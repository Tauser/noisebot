#include "nb_main_spi_transport.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nb_hw_config_main.h"
#include "nb_link_wire.h"

#ifndef CONFIG_NB_INTER_MCU_SPI_ENABLED
#define CONFIG_NB_INTER_MCU_SPI_ENABLED 0
#endif

#define NB_MAIN_SPI_TX_QUEUE_DEPTH 2U
#define NB_HEAD_RESET_LOW_MS       20U
#define NB_HEAD_RESET_SETTLE_MS    100U
#define NB_HEAD_RESET_RATE_LIMIT_MS 10000U

typedef struct {
    uint16_t length;
    uint8_t frame[NB_LINK_DM1_QUEUE_FRAME_BYTES];
} nb_spi_tx_item_t;

typedef struct {
    bool initialized;
    spi_device_handle_t device;
    QueueHandle_t tx_queue;
    StaticQueue_t tx_queue_storage;
    uint8_t tx_queue_bytes[NB_MAIN_SPI_TX_QUEUE_DEPTH *
                           sizeof(nb_spi_tx_item_t)];
    nb_main_spi_transport_config_t config;
    bool reset_seen;
    uint32_t last_reset_ms;
} nb_main_spi_context_t;

static nb_main_spi_context_t s_ctx;
static DMA_ATTR nb_link_wire_packet_t s_tx_packet;
static DMA_ATTR nb_link_wire_packet_t s_rx_packet;

esp_err_t nb_main_spi_transport_init(
    const nb_main_spi_transport_config_t *config)
{
    if (!CONFIG_NB_INTER_MCU_SPI_ENABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (config == NULL || config->on_frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = NB_MAIN_PIN_LINK_MOSI,
        .miso_io_num = NB_MAIN_PIN_LINK_MISO,
        .sclk_io_num = NB_MAIN_PIN_LINK_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)sizeof(nb_link_wire_packet_t),
    };
    spi_device_interface_config_t device = {
        .clock_speed_hz = NB_MAIN_LINK_BRINGUP_HZ,
        .mode = 0,
        .spics_io_num = NB_MAIN_PIN_LINK_CS,
        .queue_size = 1,
    };
    gpio_config_t input = {
        .pin_bit_mask = 1ULL << NB_MAIN_PIN_HEAD_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t reset = {
        .pin_bit_mask = 1ULL << NB_MAIN_PIN_HEAD_RESET,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&input);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_config(&reset);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(NB_MAIN_PIN_HEAD_RESET, 1);

    err = spi_bus_initialize(NB_MAIN_LINK_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    err = spi_bus_add_device(NB_MAIN_LINK_SPI_HOST, &device, &s_ctx.device);
    if (err != ESP_OK) {
        spi_bus_free(NB_MAIN_LINK_SPI_HOST);
        return err;
    }

    s_ctx.tx_queue = xQueueCreateStatic(
        NB_MAIN_SPI_TX_QUEUE_DEPTH,
        sizeof(nb_spi_tx_item_t),
        s_ctx.tx_queue_bytes,
        &s_ctx.tx_queue_storage);
    if (s_ctx.tx_queue == NULL) {
        spi_bus_remove_device(s_ctx.device);
        spi_bus_free(NB_MAIN_LINK_SPI_HOST);
        memset(&s_ctx, 0, sizeof(s_ctx));
        return ESP_ERR_NO_MEM;
    }

    s_ctx.config = *config;
    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t nb_main_spi_transport_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    vQueueDelete(s_ctx.tx_queue);
    esp_err_t err = spi_bus_remove_device(s_ctx.device);
    if (err == ESP_OK) {
        err = spi_bus_free(NB_MAIN_LINK_SPI_HOST);
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    return err;
}

bool nb_main_spi_transport_send(void *ctx,
                                const void *frame,
                                size_t frame_length)
{
    (void)ctx;
    nb_spi_tx_item_t item;

    if (!s_ctx.initialized || frame == NULL || frame_length == 0U ||
        frame_length > sizeof(item.frame)) {
        return false;
    }
    item.length = (uint16_t)frame_length;
    memcpy(item.frame, frame, frame_length);
    return xQueueSend(s_ctx.tx_queue, &item, 0U) == pdTRUE;
}

esp_err_t nb_main_spi_transport_poll(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    nb_spi_tx_item_t item;
    if (xQueueReceive(s_ctx.tx_queue, &item, 0U) == pdTRUE) {
        if (!nb_link_wire_pack(&s_tx_packet, item.frame, item.length)) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        nb_link_wire_clear(&s_tx_packet);
    }
    nb_link_wire_clear(&s_rx_packet);

    spi_transaction_t transaction = {
        .length = sizeof(nb_link_wire_packet_t) * 8U,
        .tx_buffer = &s_tx_packet,
        .rx_buffer = &s_rx_packet,
    };
    esp_err_t err = spi_device_transmit(s_ctx.device, &transaction);
    if (err != ESP_OK) {
        return err;
    }

    const void *frame = NULL;
    size_t frame_length = 0U;
    if (nb_link_wire_unpack(&s_rx_packet, &frame, &frame_length) &&
        frame_length > 0U) {
        s_ctx.config.on_frame(s_ctx.config.ctx, frame, frame_length);
    }
    return ESP_OK;
}

bool nb_main_spi_transport_head_irq_active(void)
{
    return s_ctx.initialized &&
           gpio_get_level(NB_MAIN_PIN_HEAD_IRQ) != 0;
}

esp_err_t nb_main_spi_transport_reset_head(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_ctx.reset_seen &&
        (uint32_t)(now_ms - s_ctx.last_reset_ms) <
            NB_HEAD_RESET_RATE_LIMIT_MS) {
        return ESP_ERR_INVALID_STATE;
    }
    gpio_set_level(NB_MAIN_PIN_HEAD_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(NB_HEAD_RESET_LOW_MS));
    gpio_set_level(NB_MAIN_PIN_HEAD_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(NB_HEAD_RESET_SETTLE_MS));
    s_ctx.reset_seen = true;
    s_ctx.last_reset_ms = now_ms;
    return ESP_OK;
}
