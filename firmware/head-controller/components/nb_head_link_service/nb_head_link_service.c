#include "nb_head_link_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nb_camera_protocol.h"
#include "nb_head_camera_service.h"
#include "nb_head_display_service.h"
#include "nb_head_spi_transport.h"
#include "nb_head_task_config.h"
#include "nb_inter_mcu_protocol.h"

#ifndef CONFIG_NB_INTER_MCU_SPI_ENABLED
#define CONFIG_NB_INTER_MCU_SPI_ENABLED 0
#endif

#define NB_HEAD_LINK_SERVICE_TIMEOUT_MS 100U
#define NB_HEAD_LINK_TELEMETRY_MS 5000U

static const char *TAG = "nb_head_link";
static nb_link_engine_t s_engine;
static bool s_initialized;
static bool s_display_capable;
static bool s_camera_capable;
static uint32_t s_spi_timeouts;
static uint32_t s_spi_errors;

/*
 * Ponte entre a task da câmera (produtor, via nb_head_link_service_send_camera_event)
 * e a task do enlace (único consumidor que chama nb_link_engine_send — o
 * engine não tem lock interno, por design é single-threaded por task).
 */
#define NB_HEAD_CAMERA_EVENT_QUEUE_DEPTH 4U
static QueueHandle_t s_camera_event_queue;
static StaticQueue_t s_camera_event_queue_storage;
static uint8_t s_camera_event_queue_bytes[
    NB_HEAD_CAMERA_EVENT_QUEUE_DEPTH * sizeof(nb_camera_link_event_t)];

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
    if (channel == NB_LINK_CHANNEL_CONTROL &&
        message_type == NB_LINK_MSG_DISPLAY_COMMAND) {
        const esp_err_t err = nb_head_display_service_apply(payload, length);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "display command rejected: %s",
                     esp_err_to_name(err));
        }
        return;
    }
    if (channel == NB_LINK_CHANNEL_CONTROL &&
        message_type == NB_LINK_MSG_CAMERA_COMMAND) {
        /* Apenas enfileira: a captura V4L2 roda na task dedicada da câmera,
         * nunca aqui — ver docs/DM4_BRINGUP.md sobre por que isso é
         * obrigatório (captura síncrona na task do enlace atrasou ACK em
         * bancada). A resposta chega depois via camera_event_sink(). */
        const esp_err_t err = nb_head_camera_service_apply(payload, length);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "camera command rejected: %s",
                     esp_err_to_name(err));
        }
        return;
    }
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

esp_err_t nb_head_link_service_send_camera_event(
    const nb_camera_link_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_camera_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_camera_event_queue, event, 0U) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void link_task(void *arg)
{
    (void)arg;
    uint32_t last_telemetry_ms = 0U;
    nb_link_engine_start(&s_engine, monotonic_ms());

    for (;;) {
        const uint32_t now_ms = monotonic_ms();
        nb_link_engine_tick(&s_engine, now_ms);

        nb_camera_link_event_t camera_event;
        while (xQueuePeek(s_camera_event_queue, &camera_event, 0U) == pdTRUE) {
            if (!nb_link_engine_send(&s_engine, NB_LINK_CHANNEL_EVENT,
                                     NB_LINK_MSG_CAMERA_EVENT, &camera_event,
                                     sizeof(camera_event))) {
                ESP_LOGW(TAG,
                         "camera event preserved during link backpressure");
                break;
            }
            (void)xQueueReceive(s_camera_event_queue, &camera_event, 0U);
        }

        const esp_err_t err = nb_head_spi_transport_service(
            pdMS_TO_TICKS(NB_HEAD_LINK_SERVICE_TIMEOUT_MS));
        if (err == ESP_ERR_TIMEOUT) {
            ++s_spi_timeouts;
        } else if (err != ESP_OK) {
            ++s_spi_errors;
            ESP_LOGW(TAG, "SPI slave falhou: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5U));
        }

        if ((uint32_t)(now_ms - last_telemetry_ms) >=
            NB_HEAD_LINK_TELEMETRY_MS) {
            const nb_link_engine_stats_t *stats =
                nb_link_engine_stats(&s_engine);
            nb_head_display_status_t display_status;
            nb_head_display_service_get_status(&display_status);
            const uint32_t ack_avg_ms =
                stats->ack_rtt_samples == 0U
                    ? 0U
                    : (uint32_t)(stats->ack_rtt_total_ms /
                                 stats->ack_rtt_samples);
            ESP_LOGI(TAG,
                     "telemetry state=%s tx=%lu rx=%lu invalid=%lu "
                     "retry=%lu timeout=%lu spi_to=%lu spi_err=%lu "
                     "hs=%lums ack_last/avg/max=%lu/%lu/%lums "
                     "display=%lu/%lu/%lu gen=%lu hw=%u/%lu "
                     "spiram=%lu",
                     state_name(nb_link_engine_state(&s_engine)),
                     (unsigned long)stats->frames_tx,
                     (unsigned long)stats->frames_rx,
                     (unsigned long)stats->dropped_invalid,
                     (unsigned long)stats->ack_retries_tx,
                     (unsigned long)stats->ack_timeouts,
                     (unsigned long)s_spi_timeouts,
                     (unsigned long)s_spi_errors,
                     (unsigned long)stats->handshake_last_ms,
                     (unsigned long)stats->ack_rtt_last_ms,
                     (unsigned long)ack_avg_ms,
                     (unsigned long)stats->ack_rtt_max_ms,
                     (unsigned long)display_status.accepted,
                     (unsigned long)display_status.rejected,
                     (unsigned long)display_status.ignored,
                     (unsigned long)(display_status.scene_valid
                                         ? display_status.scene.generation
                                         : 0U),
                     display_status.hardware_ready ? 1U : 0U,
                     (unsigned long)display_status.hardware_errors,
                     (unsigned long)display_status.spiram_free_bytes);
            last_telemetry_ms = now_ms;
        }
    }
}

esp_err_t nb_head_link_service_init(void)
{
    if (!CONFIG_NB_INTER_MCU_SPI_ENABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_engine, 0, sizeof(s_engine));
    s_camera_event_queue = xQueueCreateStatic(
        NB_HEAD_CAMERA_EVENT_QUEUE_DEPTH,
        sizeof(nb_camera_link_event_t),
        s_camera_event_queue_bytes,
        &s_camera_event_queue_storage);
    if (s_camera_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    nb_head_display_status_t display_status;
    nb_head_display_service_get_status(&display_status);
    s_display_capable = display_status.enabled;
    nb_head_camera_status_t camera_status;
    nb_head_camera_service_get_status(&camera_status);
    s_camera_capable = camera_status.enabled;
    const nb_link_engine_config_t engine_config = {
        .role = NB_LINK_ROLE_HEAD,
        .boot_id = esp_random() | 1U,
        .version_major = NB_LINK_PROTOCOL_VERSION_MAJOR,
        .version_minor = NB_LINK_PROTOCOL_VERSION_MINOR,
        .capability_bits =
            (s_display_capable ? NB_LINK_CAP_DISPLAY_SEMANTIC : 0U) |
            (s_camera_capable ? NB_LINK_CAP_CAMERA_SEMANTIC : 0U),
        .transport = {
            .send = nb_head_spi_transport_send,
            .ctx = NULL,
        },
        .on_state_change = on_state_change,
        .on_message = on_message,
        .on_tx_result = on_tx_result,
        .user_ctx = NULL,
    };
    nb_link_engine_init(&s_engine, &engine_config);

    const nb_head_spi_transport_config_t transport_config = {
        .on_frame = on_rx_frame,
        .ctx = &s_engine,
    };
    esp_err_t err = nb_head_spi_transport_init(&transport_config);
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    const BaseType_t created = xTaskCreatePinnedToCore(
        link_task,
        "nb_head_link",
        NB_TASK_HEAD_LINK_STACK,
        NULL,
        NB_TASK_HEAD_LINK_PRIORITY,
        NULL,
        NB_TASK_HEAD_LINK_CORE);
    if (created != pdPASS) {
        s_initialized = false;
        (void)nb_head_spi_transport_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "task iniciada; transporte SPI dual-MCU habilitado");
    return ESP_OK;
}

nb_link_state_t nb_head_link_service_state(void)
{
    return s_initialized ? nb_link_engine_state(&s_engine)
                         : NB_LINK_STATE_RESET;
}
