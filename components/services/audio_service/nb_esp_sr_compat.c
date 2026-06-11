/*
 * nb_esp_sr_compat.c — TU isolada que inclui esp_vad.h sem conflito.
 *
 * Este é o único arquivo do projeto que inclui esp_vad.h. Todos os outros usam
 * nb_esp_sr_compat.h para acessar as funções VAD do ESP-SR.
 */

#include "nb_esp_sr_compat.h"
#include "esp_vad.h"

nb_esp_vad_handle_t nb_esp_vad_create(int vad_mode, int sample_rate,
                                      int one_frame_ms, int min_speech_ms,
                                      int min_noise_ms)
{
    return vad_create_with_param(vad_mode, sample_rate, one_frame_ms,
                                 min_speech_ms, min_noise_ms);
}

int nb_esp_vad_process(nb_esp_vad_handle_t handle, int16_t *data,
                       int sample_rate_hz, int one_frame_ms)
{
    return vad_process(handle, data, sample_rate_hz, one_frame_ms);
}

void nb_esp_vad_reset_trigger(nb_esp_vad_handle_t handle)
{
    vad_reset_trigger(handle);
}

void nb_esp_vad_destroy(nb_esp_vad_handle_t handle)
{
    vad_destroy(handle);
}
