#ifndef NB_HEAD_SPI_TRANSPORT_H
#define NB_HEAD_SPI_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef void (*nb_head_spi_rx_fn)(void *ctx,
                                  const void *frame,
                                  size_t frame_length);

typedef struct {
    nb_head_spi_rx_fn on_frame;
    void *ctx;
} nb_head_spi_transport_config_t;

esp_err_t nb_head_spi_transport_init(
    const nb_head_spi_transport_config_t *config);
esp_err_t nb_head_spi_transport_deinit(void);

bool nb_head_spi_transport_send(void *ctx,
                                const void *frame,
                                size_t frame_length);

/* Aguarda uma transação iniciada pelo main. Chamar somente na task do link. */
esp_err_t nb_head_spi_transport_service(TickType_t timeout_ticks);

#endif /* NB_HEAD_SPI_TRANSPORT_H */
