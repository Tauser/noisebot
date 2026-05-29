/*
 * audio_processor_service.h — Probe experimental de AFE de voz (Layer 4)
 *
 * Fase 5 do voice pipeline. Este componente NÃO troca o caminho principal de
 * áudio. Ele existe para medir se um AFE_TYPE_VC em high performance cabe em
 * memória ao lado do WakeNet/AFE já ativo e para registrar parâmetros básicos
 * antes de qualquer processamento real de fala.
 *
 * NVS: namespace "nb_svc", chave "voice_afe_probe" (u8)
 *   0/ausente = desabilitado (padrão)
 *   1         = executa probe no boot
 */

#ifndef NB_AUDIO_PROCESSOR_SERVICE_H
#define NB_AUDIO_PROCESSOR_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool enabled;
    bool probe_ran;
    bool probe_ok;
    bool aec_probe_ran;
    bool aec_probe_ok;
    bool aec_supported;
    bool aec_blocked_no_reference;
    bool opus_probe_ran;
    bool opus_probe_ok;
    bool shadow_active;
    bool shadow_stop_requested;
    bool processed_bridge_enabled;
    bool processed_capture_active;
    uint32_t psram_before_kb;
    uint32_t psram_after_create_kb;
    uint32_t psram_after_destroy_kb;
    uint32_t aec_psram_before_kb;
    uint32_t aec_psram_after_create_kb;
    uint32_t aec_psram_after_destroy_kb;
    uint32_t opus_psram_before_kb;
    uint32_t opus_psram_after_create_kb;
    uint32_t opus_psram_after_destroy_kb;
    uint32_t internal_free_kb;
    uint32_t internal_largest_kb;
    uint32_t dma_free_kb;
    uint32_t dma_largest_kb;
    uint32_t shadow_psram_start_kb;
    uint32_t shadow_psram_current_kb;
    uint32_t shadow_feed_chunks;
    uint32_t shadow_fetch_chunks;
    uint32_t shadow_fetch_nulls;
    uint32_t shadow_feed_drops;
    uint32_t shadow_output_rms;
    uint16_t shadow_output_peak;
    uint32_t processed_bridge_chunks;
    uint32_t processed_bridge_fallbacks;
    uint32_t processed_output_overruns;
    uint16_t processed_buffer_level;
    int feed_chunksize;
    int fetch_chunksize;
    int feed_channels;
    int fetch_channels;
    int sample_rate_hz;
    int opus_frame_duration_ms;
    int opus_encoder_frame_bytes;
    int opus_encoder_out_bytes;
    int opus_encoded_bytes;
    int opus_decoded_bytes;
    esp_err_t last_error;
    esp_err_t aec_last_error;
    esp_err_t opus_last_error;
} nb_audio_processor_status_t;

/**
 * @brief Inicializa o serviço experimental.
 *
 * Se a flag NVS estiver desligada, apenas marca o serviço como inicializado e
 * retorna ESP_OK. Se estiver ligada, cria e destrói um AFE_TYPE_VC uma vez para
 * medir memória e dimensões de IO, sem alimentar áudio nem criar task.
 */
esp_err_t audio_processor_service_init(void);

/**
 * @brief Executa o probe uma vez, independentemente da flag NVS.
 *
 * Uso previsto: endpoint de diagnóstico ou bancada. O AFE criado aqui é
 * destruído antes do retorno.
 */
esp_err_t audio_processor_service_probe_once(void);

/**
 * @brief Executa um probe curto de AEC, sem alimentar áudio.
 *
 * Cria um AFE_TYPE_VC em formato "MR" com AEC VoIP habilitado e destrói antes
 * de retornar. Serve apenas para medir se o custo de memória cabe no firmware
 * atual antes de qualquer promoção para runtime.
 */
esp_err_t audio_processor_service_aec_probe_once(void);

/**
 * @brief Executa um probe isolado de Opus 16kHz mono 60ms.
 *
 * Abre encoder e decoder, codifica um frame PCM silencioso, decodifica o pacote
 * gerado e fecha os handles antes de retornar. Não altera o caminho real de voz.
 */
esp_err_t audio_processor_service_opus_probe_once(void);

/**
 * @brief Inicia shadow mode.
 *
 * Cria uma instância AFE_TYPE_VC persistente e uma task de fetch. A saída do
 * AFE é descartada; apenas métricas são acumuladas. Não altera VAD, bridge ou
 * STT.
 */
esp_err_t audio_processor_service_shadow_start(void);

/**
 * @brief Para shadow mode e destrói a instância AFE.
 */
esp_err_t audio_processor_service_shadow_stop(void);

/**
 * @brief Habilita o uso da saida AFE como fonte preferencial do bridge.
 *
 * Inicia shadow mode se necessario. O caminho de bridge mantém fallback para o
 * PCM original sempre que a saida processada ainda não estiver disponivel.
 */
esp_err_t audio_processor_service_bridge_start(void);

/**
 * @brief Desabilita a fonte processada do bridge sem destruir o shadow.
 */
esp_err_t audio_processor_service_bridge_stop(void);

/**
 * @brief Abre uma janela de captura processada para a sessão atual.
 *
 * Limpa o ring buffer para evitar áudio antigo antes de VOICE_START.
 */
void audio_processor_service_bridge_capture_begin(void);

/**
 * @brief Fecha a janela de captura processada.
 */
void audio_processor_service_bridge_capture_end(void);

/**
 * @brief Tenta copiar um chunk processado para o bridge.
 *
 * Retorna true quando copiou exatamente n amostras. Retorna false quando o
 * modo está desligado, sem buffer suficiente, ou temporariamente ocupado.
 */
bool audio_processor_service_read_bridge_processed(int16_t *out, uint16_t n);

/**
 * @brief Alimenta o AFE shadow com PCM mono 16kHz.
 *
 * Chamado pela audio_task. Retorna rápido se shadow mode estiver inativo.
 */
void audio_processor_service_feed_shadow(const int16_t *pcm, uint16_t n);

/**
 * @brief Copia o último status conhecido.
 */
void audio_processor_service_get_status(nb_audio_processor_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_PROCESSOR_SERVICE_H */
