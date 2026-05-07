/*
 * long_term_memory.h — Memória de longo prazo do NoiseBot (Layer 7)
 *
 * Persiste histórico de interações e estado de persona em dois arquivos
 * binários no microSD (CRC-protegidos). Carrega na inicialização,
 * acumula em RAM e faz flush periódico.
 *
 * Flush: chamar ltm_flush() apenas de tasks com prioridade < 10.
 * A behavior_task (prio 5) faz flush a cada 5min e ao entrar em SLEEPING.
 *
 * Arquivos SD:
 *   /sdcard/memory/ltm_main.bin    — persona_state + histórico 200 entradas
 *   /sdcard/memory/ltm_journal.bin — journal 1000 entradas
 *
 * Task: sem task própria — integrado ao behavior_task via ltm_tick().
 */

#ifndef NB_LONG_TERM_MEMORY_H
#define NB_LONG_TERM_MEMORY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tipos de interação ──────────────────────────────────────────────────── */

typedef enum {
    LTM_IACT_TOUCH_TAP        = 0,   /**< Toque TAP                    */
    LTM_IACT_TOUCH_LONG       = 1,   /**< Toque LONG_PRESS             */
    LTM_IACT_VOICE_START      = 2,   /**< Início de atividade de voz   */
    LTM_IACT_AUDIO_PLAYED     = 3,   /**< Áudio reproduzido            */
    LTM_IACT_SLEEP            = 4,   /**< Transição para SLEEPING      */
    LTM_IACT_WAKE             = 5,   /**< Despertar de SLEEPING        */
    LTM_IACT_TOUCH_DOUBLE_TAP = 6,   /**< Duplo toque (semântico)      */
    LTM_IACT_TOUCH_DEEP       = 7,   /**< Toque sustentado > 8s        */
    LTM_IACT_TOUCH_CARESS     = 8,   /**< Carinho > 15s (afetivo)      */
    LTM_IACT_COUNT            = 9,
} ltm_iact_type_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa a LTM.
 *
 * Tenta carregar ltm_main.bin e ltm_journal.bin do SD. Em caso de
 * arquivo ausente ou CRC inválido, inicializa com defaults zerados
 * e agenda flush ao fechar normalmente.
 *
 * Deve ser chamado após sd_hal_init() (PHASE_STORAGE) e antes de qualquer
 * chamada a ltm_record().
 *
 * @return ESP_OK sempre (falha no SD → modo sem persistência, não fatal).
 */
esp_err_t ltm_init(void);

/**
 * @brief Registra uma interação.
 *
 * Thread-safe. Adiciona ao ring buffer de histórico e ao journal.
 * Atualiza contadores em RAM (total_touch_count, familiarity_score).
 * Não escreve em SD — use ltm_flush() para persistir.
 *
 * @param type Tipo de interação.
 */
void ltm_record(ltm_iact_type_t type);

/**
 * @brief Avança o contador de uptime da sessão atual.
 *
 * Deve ser chamado pela behavior_task a cada tick (100ms).
 * Não thread-safe — chamar apenas de uma task.
 *
 * @param dt_ms Intervalo em ms desde a última chamada.
 */
void ltm_tick(uint32_t dt_ms);

/**
 * @brief Persiste estado em disco.
 *
 * Escreve ltm_main.bin e ltm_journal.bin no SD de forma síncrona.
 * Chamar apenas de tasks com prioridade < 10 (não bloqueia tasks críticas).
 *
 * No-op se SD não estiver disponível.
 */
void ltm_flush(void);

/* ── Getters ─────────────────────────────────────────────────────────────── */

/** Total de toques TAP acumulados (sessions + atual). */
uint32_t ltm_get_total_touch_count(void);

/** Número total de sessões (boots) acumuladas. */
uint32_t ltm_get_total_sessions(void);

/** Tempo total de operação em horas (arredondado para baixo). */
uint32_t ltm_get_hours_alive(void);

/**
 * @brief Conta entradas de um tipo no histórico em RAM (últimas 200).
 *
 * Thread-safe. O(LTM_HISTORY_SIZE). Usado pelo persona_service para
 * derivar as dimensões de energia e curiosidade.
 */
uint32_t ltm_count_iact(ltm_iact_type_t type);

/**
 * @brief Retorna o número de entradas válidas no ring buffer de histórico.
 *
 * Cresce de 0 até LTM_HISTORY_SIZE (200) e permanece em 200 após saturar.
 * Denominador correto para calcular frequência relativa de um tipo de
 * interação no histórico recente.
 */
uint16_t ltm_get_hist_count(void);

/**
 * @brief Retorna true quando o usuário é considerado "familiar".
 *
 * Baseado em familiarity_score ≥ 0.5.
 * Score cresce com total_touch_count: score = 1 − exp(−touches/50).
 * Familiar a partir de ~35 toques.
 */
bool ltm_is_user_familiar(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_LONG_TERM_MEMORY_H */
