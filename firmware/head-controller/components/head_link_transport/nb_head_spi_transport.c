#include "nb_head_spi_transport.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nb_hw_config_head.h"
#include "nb_link_wire.h"

#ifndef CONFIG_NB_INTER_MCU_SPI_ENABLED
#define CONFIG_NB_INTER_MCU_SPI_ENABLED 0
#endif

#define NB_HEAD_SPI_TX_QUEUE_DEPTH 2U

typedef struct {
    uint16_t length;
    uint8_t frame[NB_LINK_DM1_QUEUE_FRAME_BYTES];
} nb_spi_tx_item_t;

typedef struct {
    bool initialized;
    bool tx_pending;
    QueueHandle_t tx_queue;
    StaticQueue_t tx_queue_storage;
    uint8_t tx_queue_bytes[NB_HEAD_SPI_TX_QUEUE_DEPTH *
                           sizeof(nb_spi_tx_item_t)];
    nb_head_spi_transport_config_t config;
    nb_spi_tx_item_t pending_tx;
} nb_head_spi_context_t;

static nb_head_spi_context_t s_ctx;
static DMA_ATTR nb_link_wire_packet_t s_tx_packet;
static DMA_ATTR nb_link_wire_packet_t s_rx_packet;

esp_err_t nb_head_spi_transport_init(
    const nb_head_spi_transport_config_t *config)
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
        .mosi_io_num = NB_HEAD_PIN_LINK_MOSI,
        .miso_io_num = NB_HEAD_PIN_LINK_MISO,
        .sclk_io_num = NB_HEAD_PIN_LINK_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)sizeof(nb_link_wire_packet_t),
    };
    spi_slave_interface_config_t slave = {
        .spics_io_num = NB_HEAD_PIN_LINK_CS,
        .flags = 0,
        .queue_size = 1,
        .mode = 0,
    };
    gpio_config_t irq = {
        .pin_bit_mask = 1ULL << NB_HEAD_PIN_HEAD_IRQ,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&irq);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(NB_HEAD_PIN_HEAD_IRQ, 0);

    err = spi_slave_initialize(NB_HEAD_LINK_SPI_HOST, &bus, &slave,
                               SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.tx_queue = xQueueCreateStatic(
        NB_HEAD_SPI_TX_QUEUE_DEPTH,
        sizeof(nb_spi_tx_item_t),
        s_ctx.tx_queue_bytes,
        &s_ctx.tx_queue_storage);
    if (s_ctx.tx_queue == NULL) {
        spi_slave_free(NB_HEAD_LINK_SPI_HOST);
        memset(&s_ctx, 0, sizeof(s_ctx));
        return ESP_ERR_NO_MEM;
    }

    s_ctx.config = *config;
    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t nb_head_spi_transport_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    gpio_set_level(NB_HEAD_PIN_HEAD_IRQ, 0);
    vQueueDelete(s_ctx.tx_queue);
    esp_err_t err = spi_slave_free(NB_HEAD_LINK_SPI_HOST);
    memset(&s_ctx, 0, sizeof(s_ctx));
    return err;
}

bool nb_head_spi_transport_send(void *ctx,
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
    if (xQueueSend(s_ctx.tx_queue, &item, 0U) != pdTRUE) {
        return false;
    }
    gpio_set_level(NB_HEAD_PIN_HEAD_IRQ, 1);
    return true;
}

esp_err_t nb_head_spi_transport_service(TickType_t timeout_ticks)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_ctx.tx_pending &&
        xQueueReceive(s_ctx.tx_queue, &s_ctx.pending_tx, 0U) == pdTRUE) {
        s_ctx.tx_pending = true;
    }
    if (s_ctx.tx_pending) {
        if (!nb_link_wire_pack(&s_tx_packet,
                               s_ctx.pending_tx.frame,
                               s_ctx.pending_tx.length)) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        nb_link_wire_clear(&s_tx_packet);
        gpio_set_level(NB_HEAD_PIN_HEAD_IRQ, 0);
    }
    nb_link_wire_clear(&s_rx_packet);

    spi_slave_transaction_t transaction = {
        .length = sizeof(nb_link_wire_packet_t) * 8U,
        .tx_buffer = &s_tx_packet,
        .rx_buffer = &s_rx_packet,
    };
    esp_err_t err = spi_slave_transmit(NB_HEAD_LINK_SPI_HOST, &transaction,
                                       timeout_ticks);
    if (err != ESP_OK) {
        return err;
    }
    s_ctx.tx_pending = false;

    if (!s_ctx.tx_pending && uxQueueMessagesWaiting(s_ctx.tx_queue) == 0U) {
        gpio_set_level(NB_HEAD_PIN_HEAD_IRQ, 0);
    }

    const void *frame = NULL;
    size_t frame_length = 0U;
    if (nb_link_wire_unpack(&s_rx_packet, &frame, &frame_length) &&
        frame_length > 0U) {
        s_ctx.config.on_frame(s_ctx.config.ctx, frame, frame_length);
    }
    return ESP_OK;
}
