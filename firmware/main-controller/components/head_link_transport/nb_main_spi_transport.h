#ifndef NB_MAIN_SPI_TRANSPORT_H
#define NB_MAIN_SPI_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef void (*nb_main_spi_rx_fn)(void *ctx,
                                  const void *frame,
                                  size_t frame_length);

typedef struct {
    nb_main_spi_rx_fn on_frame;
    void *ctx;
} nb_main_spi_transport_config_t;

esp_err_t nb_main_spi_transport_init(
    const nb_main_spi_transport_config_t *config);
esp_err_t nb_main_spi_transport_deinit(void);

bool nb_main_spi_transport_send(void *ctx,
                                const void *frame,
                                size_t frame_length);

/* Executa uma transação full-duplex. Chamar pela task do enlace; nunca por ISR. */
esp_err_t nb_main_spi_transport_poll(void);

bool nb_main_spi_transport_head_irq_active(void);
esp_err_t nb_main_spi_transport_reset_head(void);

#endif /* NB_MAIN_SPI_TRANSPORT_H */
