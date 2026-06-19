/*
 * persistence_mgr.h — Gerenciador de persistência do NoiseBot (Layer 2)
 *
 * Abstração unificada sobre NVS e microSD para operações de persistência.
 * Responsabilidades:
 *
 *   - Iniciar persistence_task (Core 0, prioridade 5) que processa a fila
 *     de escritas assíncronas.
 *   - Capturar saída do ESP_LOG e redirecionar para SD via fila.
 *   - Gerenciar re-mount periódico do SD (a cada NB_SD_REMOUNT_INTERVAL_S).
 *   - Modo SD-degradado: operações enfileiradas são descartadas silenciosamente
 *     quando SD não está disponível. Nenhum comportamento critico depende do SD.
 *
 * Hierarquia de escrita:
 *   Urgente  (crash dump)     → sd_hal diretamente, síncrono
 *   Normal   (log, memória)   → persistence_mgr_enqueue_log() → fila → task
 *
 * Task:
 *   Nome:     "nb_persist"
 *   Core:     0
 *   Priority: 5
 *   Stack:    4096 bytes
 */

#ifndef NB_PERSISTENCE_MGR_H
#define NB_PERSISTENCE_MGR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* ── Init ────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o gerenciador de persistência.
 *
 * - Cria a fila de operações.
 * - Cria nb_persist_task.
 * - Instala hook de log ESP-IDF (esp_log_set_vprintf).
 * - Ativa escrita em SD se sd_hal_is_mounted().
 *
 * Deve ser chamado após sd_hal_init() em PHASE_STORAGE.
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_NO_MEM se não houver memória para a fila/task.
 */
esp_err_t persistence_mgr_init(void);

/**
 * @brief Retorna true se SD está disponível e escrita ativa.
 */
bool persistence_mgr_is_sd_available(void);

/* ── Escrita assíncrona ───────────────────────────────────────────────────── */

/**
 * @brief Enfileira uma linha de log para escrita no SD.
 *
 * Não bloqueia. Se a fila estiver cheia, o item é descartado.
 * Chamado internamente pelo hook de vprintf — não chamar diretamente.
 *
 * @param line Ponteiro para o texto (não precisa ser nulo-terminado).
 * @param len  Comprimento em bytes.
 */
void persistence_mgr_enqueue_log(const char *line, size_t len);

/**
 * @brief Força flush síncrono de operações pendentes.
 *
 * Bloqueia até a fila esvaziar ou timeout de 5 segundos.
 * Chamar antes de entrar em SLEEPING ou em shutdown controlado.
 */
void persistence_mgr_flush_sync(void);

#endif /* NB_PERSISTENCE_MGR_H */
