/*
 * board_caps.h - Capacidades fixas da placa NoiseBot atual (Layer 1)
 */

#ifndef NB_BOARD_CAPS_H
#define NB_BOARD_CAPS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *board_name;
    uint16_t audio_sample_rate_hz;
    uint8_t mic_channels;
    uint8_t speaker_channels;
    bool audio_full_duplex;
    bool audio_input_reference;
    bool supports_device_aec;
    bool supports_wake_word;
    bool supports_camera;
    bool supports_i2c_bus;
} nb_board_caps_t;

/**
 * @brief Retorna as capacidades do hardware ativo.
 *
 * AEC no dispositivo exige um canal limpo de referencia do speaker. O hardware
 * atual usa INMP441 + MAX98357A sem loopback/reference channel, portanto
 * supports_device_aec permanece false neste perfil.
 *
 * supports_i2c_bus reflete apenas a existencia do barramento (pinos 4/5).
 * Sensores individuais (IMU, bateria, proximidade...) nao sao presumidos —
 * ver nb_i2c_hal_get_last_scan()/GET /api/i2c para o que foi detectado.
 */
const nb_board_caps_t *nb_board_caps_get(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_BOARD_CAPS_H */
