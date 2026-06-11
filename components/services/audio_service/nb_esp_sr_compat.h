/*
 * nb_esp_sr_compat.h — Wrapper para ESP-SR VAD sem conflito de nomes.
 *
 * Expõe as quatro funções da API ESP-SR necessárias ao audio_service com nomes
 * próprios (nb_esp_vad_*), isolando o conflito com os tipos locais de audio_service.c
 * (vad_state_t / VAD_SILENCE também definidos em esp_vad.h para ESP32-S3).
 *
 * Apenas nb_esp_sr_compat.c inclui esp_vad.h. Nenhum outro arquivo deve fazê-lo.
 */

#ifndef NB_ESP_SR_COMPAT_H
#define NB_ESP_SR_COMPAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *nb_esp_vad_handle_t;

nb_esp_vad_handle_t nb_esp_vad_create(int vad_mode, int sample_rate,
                                      int one_frame_ms, int min_speech_ms,
                                      int min_noise_ms);
int  nb_esp_vad_process(nb_esp_vad_handle_t handle, int16_t *data,
                        int sample_rate_hz, int one_frame_ms);
void nb_esp_vad_reset_trigger(nb_esp_vad_handle_t handle);
void nb_esp_vad_destroy(nb_esp_vad_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* NB_ESP_SR_COMPAT_H */
