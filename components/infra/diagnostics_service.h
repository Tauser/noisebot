/*
 * diagnostics_service.h — Observabilidade do sistema NoiseBot (Layer 2)
 *
 * Coleta e expõe métricas de saúde do sistema: heap, FPS, stack watermarks.
 * Calcula um health_score composto (0–100) e publica eventos quando há
 * degradação significativa.
 *
 * Substitui o stats_dump() avulso do boot_manager, centralizando toda
 * a lógica de diagnóstico com API limpa.
 *
 * Dependências: event_bus, esp_heap_caps, FreeRTOS task stats.
 * FPS é injetado via callback em diagnostics_init() — sem dependência
 * circular para Layer 4 (render_service).
 *
 * Thread-safety: getters são thread-safe via spinlock.
 * collect() e dump_to_sd() devem ser chamados apenas de tasks com
 * prioridade < 10 (não são caminhos críticos).
 *
 * Task: sem task própria — integrado ao behavior_task via diagnostics_collect().
 */

#ifndef NB_DIAGNOSTICS_SERVICE_H
#define NB_DIAGNOSTICS_SERVICE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Threshold de health score para evento de aviso ─────────────────────── */

#define NB_DIAG_HEALTH_WARN_THRESHOLD   50U   /**< score < 50 → NB_EVT_HEALTH_WARNING */
#define NB_DIAG_HEAP_LOW_PSRAM_KB      300U   /**< PSRAM < 300KB → NB_EVT_HEAP_LOW    */

/* ── Injeção de providers externos (evita dependência Layer 2 → Layer 4) ── */

/**
 * Provider de FPS injetado pelo boot_manager.
 * Aponta para render_service_get_fps() sem que este header dependa de
 * render_service.h.
 */
typedef float (*nb_diag_fps_provider_t)(void);

typedef struct {
    nb_diag_fps_provider_t get_fps;  /**< Pode ser NULL → FPS reportado como 0. */
} nb_diagnostics_config_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o diagnostics_service.
 *
 * Deve ser chamado após event_bus_init() e antes do primeiro
 * diagnostics_collect().
 *
 * @param cfg  Configuração com providers opcionais. Não pode ser NULL.
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t diagnostics_init(const nb_diagnostics_config_t *cfg);

/**
 * @brief Coleta snapshot de métricas e avalia health_score.
 *
 * Deve ser chamado pelo behavior_task a cada 60s.
 * Publica NB_EVT_HEALTH_WARNING se score < threshold.
 * Publica NB_EVT_HEAP_LOW se PSRAM < threshold.
 * Thread-unsafe — chamar de uma única task.
 */
void diagnostics_collect(void);

/**
 * @brief Grava snapshot de diagnóstico em /sdcard/logs/diag_<uptime>s.txt.
 *
 * No-op silencioso se SD não estiver disponível (fopen retorna NULL).
 * Deve ser chamado de task com prioridade < 10.
 */
void diagnostics_dump_to_sd(void);

/* ── Getters (thread-safe) ───────────────────────────────────────────────── */

/** Score de saúde do sistema [0, 100]. 100 = tudo OK. */
uint8_t  diagnostics_get_health_score(void);

/** Último FPS medido pelo render_service. Zero antes da primeira coleta. */
float    diagnostics_get_fps(void);

/** Memória livre em bytes para o capability fornecido (ex: MALLOC_CAP_SPIRAM). */
uint32_t diagnostics_get_heap_free(uint32_t caps);

/** Tempo de operação em segundos desde o boot. */
uint32_t diagnostics_get_uptime_s(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_DIAGNOSTICS_SERVICE_H */
