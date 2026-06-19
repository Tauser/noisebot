#include "nb_main_link_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nb_inter_mcu_protocol.h"
#include "nb_main_spi_transport.h"
#include "nb_task_config.h"

#ifndef CONFIG_NB_INTER_MCU_SPI_ENABLED
#define CONFIG_NB_INTER_MCU_SPI_ENABLED 0
#endif

#define NB_MAIN_LINK_LOOP_MS 5U
#define NB_MAIN_LINK_POLL_MS 20U

static const char *TAG = "nb_main_link";
static nb_link_engine_t s_engine;
static bool s_initialized;

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *state_name(nb_link_state_t state)
{
    switch (state) {
        case NB_LINK_STATE_RESET: return "RESET";
        case NB_LINK_STATE_HANDSHAKE: return "HANDSHAKE";
        case NB_LINK_STATE_SNAPSHOT: return "SNAPSHOT";
        case NB_LINK_STATE_READY: return "READY";
        case NB_LINK_STATE_DEGRADED: return "DEGRADED";
        default: return "UNKNOWN";
    }
}

static void on_rx_frame(void *ctx, const void *frame, size_t frame_length)
{
    nb_link_engine_on_frame((nb_link_engine_t *)ctx,
                            frame,
                            frame_length);
}

static void on_state_change(void *ctx,
                            nb_link_state_t previous,
                            nb_link_state_t current)
{
    (void)ctx;
    ESP_LOGI(TAG, "state %s -> %s",
             state_name(previous), state_name(current));
}

static void on_message(void *ctx,
                       nb_link_channel_t channel,
                       uint16_t message_type,
                       const void *payload,
                       uint16_t length)
{
    (void)ctx;
    (void)payload;
    ESP_LOGD(TAG, "application message deferred channel=%u type=%u bytes=%u",
             (unsigned)channel, (unsigned)message_type, (unsigned)length);
}

static void on_tx_result(void *ctx,
                         nb_link_tx_result_t result,
                         nb_link_channel_t channel,
                         uint16_t message_type,
                         uint32_t sequence)
{
    (void)ctx;
    if (result != NB_LINK_TX_ACKED) {
        ESP_LOGW(TAG, "tx result=%u channel=%u type=%u sequence=%lu",
                 (unsigned)result, (unsigned)channel, (unsigned)message_type,
                 (unsigned long)sequence);
    }
}

static void link_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t last_poll_ms = 0U;

    nb_link_engine_start(&s_engine, monotonic_ms());

    for (;;) {
        const uint32_t now_ms = monotonic_ms();
        nb_link_engine_tick(&s_engine, now_ms);

        if (nb_main_spi_transport_head_irq_active() ||
            (uint32_t)(now_ms - last_poll_ms) >= NB_MAIN_LINK_POLL_MS) {
            const esp_err_t err = nb_main_spi_transport_poll();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "SPI poll falhou: %s", esp_err_to_name(err));
            }
            last_poll_ms = now_ms;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(NB_MAIN_LINK_LOOP_MS));
    }
}

esp_err_t nb_main_link_service_init(void)
{
    if (!CONFIG_NB_INTER_MCU_SPI_ENABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_engine, 0, sizeof(s_engine));
    const nb_link_engine_config_t engine_config = {
        .role = NB_LINK_ROLE_MAIN,
        .boot_id = esp_random() | 1U,
        .version_major = NB_LINK_PROTOCOL_VERSION_MAJOR,
        .version_minor = NB_LINK_PROTOCOL_VERSION_MINOR,
        .capability_bits = 0U,
        .transport = {
            .send = nb_main_spi_transport_send,
            .ctx = NULL,
        },
        .on_state_change = on_state_change,
        .on_message = on_message,
        .on_tx_result = on_tx_result,
        .user_ctx = NULL,
    };
    nb_link_engine_init(&s_engine, &engine_config);

    const nb_main_spi_transport_config_t transport_config = {
        .on_frame = on_rx_frame,
        .ctx = &s_engine,
    };
    esp_err_t err = nb_main_spi_transport_init(&transport_config);
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    const BaseType_t created = xTaskCreatePinnedToCore(
        link_task,
        "nb_main_link",
        NB_TASK_MAIN_LINK_STACK,
        NULL,
        NB_TASK_MAIN_LINK_PRIORITY,
        NULL,
        NB_TASK_MAIN_LINK_CORE);
    if (created != pdPASS) {
        s_initialized = false;
        (void)nb_main_spi_transport_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "task iniciada; transporte SPI dual-MCU habilitado");
    return ESP_OK;
}

nb_link_state_t nb_main_link_service_state(void)
{
    return s_initialized ? nb_link_engine_state(&s_engine)
                         : NB_LINK_STATE_RESET;
}
