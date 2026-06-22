#include "nb_main_link_service.h"

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
#include "nb_display_protocol.h"
#include "nb_inter_mcu_protocol.h"
#include "nb_main_spi_transport.h"
#include "nb_task_config.h"

#ifndef CONFIG_NB_INTER_MCU_SPI_ENABLED
#define CONFIG_NB_INTER_MCU_SPI_ENABLED 0
#endif

#define NB_MAIN_LINK_LOOP_MS 5U
#define NB_MAIN_LINK_POLL_MS 10U
#define NB_MAIN_LINK_TELEMETRY_MS 5000U
#define NB_MAIN_DISPLAY_QUEUE_DEPTH 1U
#define NB_MAIN_SCENE_V2_QUEUE_DEPTH 1U
#define NB_MAIN_TRANSIENT_V2_QUEUE_DEPTH 4U
#define NB_MAIN_STATUS_ICONS_V2_QUEUE_DEPTH 1U

static const char *TAG = "nb_main_link";
static nb_link_engine_t s_engine;
static bool s_initialized;
static uint32_t s_spi_errors;
static QueueHandle_t s_display_queue;
static portMUX_TYPE s_display_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_display_snapshot_valid;
static nb_display_command_t s_display_snapshot;
static portMUX_TYPE s_camera_event_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_camera_event_valid;
static nb_camera_link_event_t s_camera_event;
static QueueHandle_t s_scene_v2_queue;
static QueueHandle_t s_transient_v2_queue;
static QueueHandle_t s_status_icons_v2_queue;
static portMUX_TYPE s_result_v2_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_result_v2_valid;
static nb_display_result_v2_t s_result_v2;

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

    if (current == NB_LINK_STATE_READY && s_display_queue != NULL) {
        nb_display_command_t snapshot;
        bool snapshot_valid;
        taskENTER_CRITICAL(&s_display_snapshot_lock);
        snapshot_valid = s_display_snapshot_valid;
        snapshot = s_display_snapshot;
        taskEXIT_CRITICAL(&s_display_snapshot_lock);

        if (snapshot_valid) {
            (void)xQueueReset(s_display_queue);
            if (xQueueSendToFront(s_display_queue, &snapshot, 0U) == pdTRUE) {
                ESP_LOGI(TAG,
                         "snapshot visual restaurado generation=%lu",
                         (unsigned long)snapshot.generation);
            } else {
                ESP_LOGE(TAG, "falha ao restaurar snapshot visual");
            }
        }
    }
}

static void on_message(void *ctx,
                       nb_link_channel_t channel,
                       uint16_t message_type,
                       const void *payload,
                       uint16_t length)
{
    (void)ctx;
    if (channel == NB_LINK_CHANNEL_EVENT &&
        message_type == NB_LINK_MSG_CAMERA_EVENT) {
        if (nb_camera_link_event_is_valid(
                (const nb_camera_link_event_t *)payload, length)) {
            taskENTER_CRITICAL(&s_camera_event_lock);
            memcpy(&s_camera_event, payload, sizeof(s_camera_event));
            s_camera_event_valid = true;
            taskEXIT_CRITICAL(&s_camera_event_lock);
        } else {
            ESP_LOGW(TAG, "camera event invalid bytes=%u", (unsigned)length);
        }
        return;
    }
    if (channel == NB_LINK_CHANNEL_EVENT &&
        message_type == NB_LINK_MSG_DISPLAY_RESULT_V2) {
        if (nb_display_result_v2_is_valid(
                (const nb_display_result_v2_t *)payload, length)) {
            taskENTER_CRITICAL(&s_result_v2_lock);
            memcpy(&s_result_v2, payload, sizeof(s_result_v2));
            s_result_v2_valid = true;
            taskEXIT_CRITICAL(&s_result_v2_lock);
        } else {
            ESP_LOGW(TAG, "visual result v2 invalid bytes=%u",
                     (unsigned)length);
        }
        return;
    }
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
    uint32_t last_telemetry_ms = 0U;

    nb_link_engine_start(&s_engine, monotonic_ms());

    for (;;) {
        const uint32_t now_ms = monotonic_ms();
        nb_link_engine_tick(&s_engine, now_ms);

        nb_display_command_t display_command;
        while (xQueuePeek(s_display_queue, &display_command, 0U) == pdTRUE) {
            if (!nb_link_engine_send(&s_engine,
                                     NB_LINK_CHANNEL_CONTROL,
                                     NB_LINK_MSG_DISPLAY_COMMAND,
                                     &display_command,
                                     sizeof(display_command))) {
                ESP_LOGW(TAG,
                         "display command preserved during link backpressure");
                break;
            }
            (void)xQueueReceive(s_display_queue, &display_command, 0U);
        }

        if (nb_link_engine_peer_has_capability(&s_engine,
                                               NB_LINK_CAP_DISPLAY_V2)) {
            nb_display_scene_v2_t scene_v2;
            while (xQueuePeek(s_scene_v2_queue, &scene_v2, 0U) == pdTRUE) {
                if (!nb_link_engine_send(&s_engine,
                                         NB_LINK_CHANNEL_CONTROL,
                                         NB_LINK_MSG_DISPLAY_SCENE_V2,
                                         &scene_v2,
                                         sizeof(scene_v2))) {
                    ESP_LOGW(TAG,
                             "scene v2 preserved during link backpressure");
                    break;
                }
                (void)xQueueReceive(s_scene_v2_queue, &scene_v2, 0U);
            }

            nb_display_transient_v2_t transient_v2;
            while (xQueuePeek(s_transient_v2_queue, &transient_v2, 0U) ==
                   pdTRUE) {
                if (!nb_link_engine_send(&s_engine,
                                         NB_LINK_CHANNEL_CONTROL,
                                         NB_LINK_MSG_DISPLAY_TRANSIENT_V2,
                                         &transient_v2,
                                         sizeof(transient_v2))) {
                    ESP_LOGW(TAG,
                             "transient v2 id=%lu preserved during "
                             "link backpressure",
                             (unsigned long)transient_v2.transient_id);
                    break;
                }
                (void)xQueueReceive(s_transient_v2_queue, &transient_v2, 0U);
            }

            nb_display_status_icons_v2_t status_icons_v2;
            while (xQueuePeek(s_status_icons_v2_queue, &status_icons_v2,
                              0U) == pdTRUE) {
                if (!nb_link_engine_send(&s_engine,
                                         NB_LINK_CHANNEL_CONTROL,
                                         NB_LINK_MSG_DISPLAY_STATUS_ICONS_V2,
                                         &status_icons_v2,
                                         sizeof(status_icons_v2))) {
                    ESP_LOGW(TAG,
                             "status icons v2 preserved during link "
                             "backpressure");
                    break;
                }
                (void)xQueueReceive(s_status_icons_v2_queue,
                                    &status_icons_v2, 0U);
            }
        }

        if (nb_main_spi_transport_head_irq_active() ||
            (uint32_t)(now_ms - last_poll_ms) >= NB_MAIN_LINK_POLL_MS) {
            const esp_err_t err = nb_main_spi_transport_poll();
            if (err != ESP_OK) {
                ++s_spi_errors;
                ESP_LOGW(TAG, "SPI poll falhou: %s", esp_err_to_name(err));
            }
            last_poll_ms = now_ms;
        }

        if ((uint32_t)(now_ms - last_telemetry_ms) >=
            NB_MAIN_LINK_TELEMETRY_MS) {
            const nb_link_engine_stats_t *stats =
                nb_link_engine_stats(&s_engine);
            const uint32_t ack_avg_ms =
                stats->ack_rtt_samples == 0U
                    ? 0U
                    : (uint32_t)(stats->ack_rtt_total_ms /
                                 stats->ack_rtt_samples);
            ESP_LOGI(TAG,
                     "telemetry state=%s tx=%lu rx=%lu invalid=%lu "
                     "retry=%lu timeout=%lu spi_err=%lu hs=%lums "
                     "ack_last/avg/max=%lu/%lu/%lums",
                     state_name(nb_link_engine_state(&s_engine)),
                     (unsigned long)stats->frames_tx,
                     (unsigned long)stats->frames_rx,
                     (unsigned long)stats->dropped_invalid,
                     (unsigned long)stats->ack_retries_tx,
                     (unsigned long)stats->ack_timeouts,
                     (unsigned long)s_spi_errors,
                     (unsigned long)stats->handshake_last_ms,
                     (unsigned long)stats->ack_rtt_last_ms,
                     (unsigned long)ack_avg_ms,
                     (unsigned long)stats->ack_rtt_max_ms);
            last_telemetry_ms = now_ms;
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
    s_display_queue = xQueueCreate(NB_MAIN_DISPLAY_QUEUE_DEPTH,
                                   sizeof(nb_display_command_t));
    s_scene_v2_queue = xQueueCreate(NB_MAIN_SCENE_V2_QUEUE_DEPTH,
                                    sizeof(nb_display_scene_v2_t));
    s_transient_v2_queue = xQueueCreate(NB_MAIN_TRANSIENT_V2_QUEUE_DEPTH,
                                        sizeof(nb_display_transient_v2_t));
    s_status_icons_v2_queue = xQueueCreate(
        NB_MAIN_STATUS_ICONS_V2_QUEUE_DEPTH,
        sizeof(nb_display_status_icons_v2_t));
    if (s_display_queue == NULL || s_scene_v2_queue == NULL ||
        s_transient_v2_queue == NULL || s_status_icons_v2_queue == NULL) {
        if (s_display_queue != NULL) vQueueDelete(s_display_queue);
        if (s_scene_v2_queue != NULL) vQueueDelete(s_scene_v2_queue);
        if (s_transient_v2_queue != NULL) vQueueDelete(s_transient_v2_queue);
        if (s_status_icons_v2_queue != NULL) vQueueDelete(s_status_icons_v2_queue);
        s_display_queue = NULL;
        s_scene_v2_queue = NULL;
        s_transient_v2_queue = NULL;
        s_status_icons_v2_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
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
        vQueueDelete(s_display_queue);
        vQueueDelete(s_scene_v2_queue);
        vQueueDelete(s_transient_v2_queue);
        vQueueDelete(s_status_icons_v2_queue);
        s_display_queue = NULL;
        s_scene_v2_queue = NULL;
        s_transient_v2_queue = NULL;
        s_status_icons_v2_queue = NULL;
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
        vQueueDelete(s_display_queue);
        vQueueDelete(s_scene_v2_queue);
        vQueueDelete(s_transient_v2_queue);
        vQueueDelete(s_status_icons_v2_queue);
        s_display_queue = NULL;
        s_scene_v2_queue = NULL;
        s_transient_v2_queue = NULL;
        s_status_icons_v2_queue = NULL;
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

esp_err_t nb_main_link_service_reset_head(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return nb_main_spi_transport_reset_head();
}

esp_err_t nb_main_link_service_queue_display(
    const nb_display_command_t *command)
{
    if (!nb_display_command_is_valid(command, sizeof(*command))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_display_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_display_snapshot_lock);
    s_display_snapshot = *command;
    s_display_snapshot_valid = true;
    taskEXIT_CRITICAL(&s_display_snapshot_lock);

    if (nb_link_engine_state(&s_engine) != NB_LINK_STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nb_link_engine_peer_has_capability(
            &s_engine, NB_LINK_CAP_DISPLAY_SEMANTIC)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return xQueueOverwrite(s_display_queue, command) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t nb_main_link_service_request_camera(
    const nb_camera_link_command_t *command)
{
    if (!nb_camera_link_command_is_valid(command, sizeof(*command))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (nb_link_engine_state(&s_engine) != NB_LINK_STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nb_link_engine_peer_has_capability(
            &s_engine, NB_LINK_CAP_CAMERA_SEMANTIC)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return nb_link_engine_send(&s_engine, NB_LINK_CHANNEL_CONTROL,
                               NB_LINK_MSG_CAMERA_COMMAND, command,
                               sizeof(*command))
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t nb_main_link_service_get_last_camera_event(
    nb_camera_link_event_t *out_event)
{
    if (out_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_ERR_NOT_FOUND;
    taskENTER_CRITICAL(&s_camera_event_lock);
    if (s_camera_event_valid) {
        *out_event = s_camera_event;
        result = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_camera_event_lock);
    return result;
}

esp_err_t nb_main_link_service_request_visual_scene_v2(
    const nb_display_scene_v2_t *scene)
{
    if (!nb_display_scene_v2_is_valid(scene, sizeof(*scene))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_scene_v2_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nb_link_engine_peer_has_capability(&s_engine,
                                            NB_LINK_CAP_DISPLAY_V2)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Igual ao v1: snapshot idempotente, overwrite e' o comportamento
     * correto (generation mais nova sempre vence). Quem chama
     * nb_link_engine_send e' so a link_task. */
    return xQueueOverwrite(s_scene_v2_queue, scene) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t nb_main_link_service_request_visual_transient_v2(
    const nb_display_transient_v2_t *command)
{
    if (!nb_display_transient_v2_is_valid(command, sizeof(*command))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_transient_v2_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nb_link_engine_peer_has_capability(&s_engine,
                                            NB_LINK_CAP_DISPLAY_V2)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Fila cheia retorna ESP_ERR_NO_MEM (backpressure explicito) -- nunca
     * sobrescreve um transiente em voo, ao contrario do scene. */
    return xQueueSend(s_transient_v2_queue, command, 0U) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t nb_main_link_service_get_last_visual_result_v2(
    nb_display_result_v2_t *out_result)
{
    if (out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_ERR_NOT_FOUND;
    taskENTER_CRITICAL(&s_result_v2_lock);
    if (s_result_v2_valid) {
        *out_result = s_result_v2;
        result = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_result_v2_lock);
    return result;
}

esp_err_t nb_main_link_service_set_status_icons_v2(uint32_t icon_bits)
{
    if (!s_initialized || s_status_icons_v2_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nb_link_engine_peer_has_capability(&s_engine,
                                            NB_LINK_CAP_DISPLAY_V2)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const nb_display_status_icons_v2_t icons = {
        .version = NB_DISPLAY_STATUS_ICONS_V2_VERSION,
        .reserved = {0U, 0U, 0U},
        .icon_bits = icon_bits,
    };
    return xQueueOverwrite(s_status_icons_v2_queue, &icons) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}
