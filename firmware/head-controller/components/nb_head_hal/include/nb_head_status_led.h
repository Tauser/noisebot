#ifndef NB_HEAD_STATUS_LED_H
#define NB_HEAD_STATUS_LED_H

#include "esp_err.h"

/**
 * @brief Inicializa o LED RGB endereçável onboard (WS2812, GPIO48) e o apaga.
 *
 * Sem isso o chip fica num estado indefinido (sem frame RMT valido na linha
 * de dados) e tende a mostrar branco no brilho maximo. Chamar uma vez no
 * boot, antes de qualquer overlay visual depender desse LED.
 */
esp_err_t nb_head_status_led_init(void);

#endif /* NB_HEAD_STATUS_LED_H */
