/*
 * wake_service.h — Wake Word via ESP-SR WakeNet (Layer 4)
 *
 * Wrapper sobre AFE (Audio Front End) + WakeNet9.
 * Wake word padrão: "Hi ESP" (modelo pronto, sem treinamento).
 *
 * Pipeline:
 *   audio_hal → wake_service_feed() → AFE.feed() → [wake_task] →
 *   AFE.fetch() → WAKENET_DETECTED → NB_EVT_WAKE_WORD_DETECTED
 *
 * Integração com audio_service:
 *   audio_task chama wake_service_feed(s_sa_buf, mic_n) após cada chunk RX.
 *   wake_service acumula amostras até feed_chunksize (tipicamente 512 @ 16kHz)
 *   e entrega ao AFE. A task dedicada (Core 0, prio 7) faz o fetch bloqueante.
 *
 * NVS: "nb_svc" / "ww_enabled" (u8) — 0 = desabilitado, padrão = habilitado.
 *   Desabilitar sem reflash: nvs_set_u8(hdl, "ww_enabled", 0).
 *
 * Investigação de memória (12.6):
 *   Log no init reporta PSRAM livre pré/pós AFE.
 *   Critério: manter > 300 KB após init.
 *
 * Task: "nb_wake_task"  Core: 0  Prioridade: 7  Stack: 4096
 */

#ifndef NB_WAKE_SERVICE_H
#define NB_WAKE_SERVICE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o wake_service.
 *
 * Lê NVS ww_enabled. Se habilitado: cria AFE, mede PSRAM, cria task.
 * Deve ser chamado em PHASE_SERVICES, após NVS disponível.
 *
 * @return ESP_OK, ESP_FAIL (AFE create), ESP_ERR_NO_MEM (task).
 */
esp_err_t wake_service_init(void);

/**
 * @brief Alimenta o AFE com um chunk de áudio do microfone.
 *
 * Chamado pela audio_task a cada ciclo (não-bloqueante).
 * Acumula amostras internamente até feed_chunksize antes de chamar AFE.feed().
 *
 * @param pcm  Buffer int16 mono 16kHz (s_sa_buf da audio_task).
 * @param n    Número de amostras (tipicamente NB_AUDIO_CHUNK_FRAMES = 256).
 */
void wake_service_feed(const int16_t *pcm, uint16_t n);

/**
 * @brief Retorna true se WakeNet está ativo (init OK e ww_enabled).
 */
bool wake_service_is_active(void);

/**
 * @brief Número de wake words detectadas nesta sessão (desde o boot).
 * Thread-safe.
 */
uint32_t wake_service_get_detect_count(void);

/**
 * @brief Threshold de detecção configurado (0.0–1.0).
 */
float wake_service_get_threshold(void);

/**
 * @brief Suspende temporariamente o WakeNet e limpa o buffer do AFE.
 *
 * Usado enquanto uma sessão de escuta está ativa para que a fala do comando
 * não seja interpretada como nova wake word.
 */
void wake_service_suspend(void);

/**
 * @brief Rearma o WakeNet após uma sessão de escuta.
 *
 * Limpa áudio residual no AFE e habilita novamente o WakeNet para a próxima
 * frase "Hi ESP".
 */
void wake_service_rearm(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_WAKE_SERVICE_H */
