/*
 * boredom_service.h — Escalada criativa de ociosidade do NoiseBot (Layer 6)
 *
 * Rastreia tempo sem interação e dispara reações progressivas quando o robô
 * fica ignorado por muito tempo. Todas as reações são transitórias e usam
 * expression_play() — nunca substituem o baseline IDLE.
 *
 * Escalada (a partir do fim do grace period):
 *   Nível 1 — CURIOUS    após NB_BOREDOM_LEVEL1_MS  sem interação
 *   Nível 2 — SAD        após NB_BOREDOM_LEVEL2_MS
 *   Nível 3 — SUSPICIOUS após NB_BOREDOM_LEVEL3_MS
 *   Nível 4 — ANGRY      após NB_BOREDOM_LEVEL4_MS  (teatral, transitório)
 *   Demônio — ANGRY long após NB_BOREDOM_DEMON_ELIGIBLE_MS (5% por check,
 *              cooldown NB_BOREDOM_DEMON_CD_MS)
 *
 * Cooldowns:
 *   Reações comuns (níveis 1-4): NB_BOREDOM_REACTION_CD_MS entre disparos.
 *   Modo demônio: NB_BOREDOM_DEMON_CD_MS separado, mais longo.
 *
 * Cancelamento: qualquer interação (touch, voz, wake word, bridge event,
 * state change relevante) chama boredom_service_on_interaction() e reinicia
 * o contador de tempo ocioso do zero.
 *
 * Pausas: serviço fica inativo enquanto o robô estiver nos estados
 * SLEEPING, MEDITATION, SILENT_COMPANY, RESPONDING ou ERROR, ou quando
 * behavior_engine declarar presença/ausência social calma.
 *
 * Restrições de arquitetura:
 *   - Layer 6 — chama somente serviços de Layer 4-5.
 *   - Não publica no event bus; as reações são self-contained.
 *   - Nenhum malloc em caminho crítico — todos os buffers são estáticos.
 *   - Timer esp_timer dispara check a cada NB_BOREDOM_CHECK_INTERVAL_S.
 *
 * Threads:
 *   - boredom_service_on_interaction() — qualquer task (spinlock interno)
 *   - boredom_service_set_paused()     — qualquer task (spinlock interno)
 *   - boredom_service_set_social_paused() — qualquer task (spinlock interno)
 *   - nb_boredom_task                  — task interna (prio 3, stack 4096)
 */

#ifndef NB_BOREDOM_SERVICE_H
#define NB_BOREDOM_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Timing ──────────────────────────────────────────────────────────────── */

/** Grace period após boot: sem reações neste intervalo. */
#define NB_BOREDOM_GRACE_MS          (2UL  * 60UL * 1000UL)

/** Após quanto tempo ocioso cada nível de escalada dispara. */
#define NB_BOREDOM_LEVEL1_MS         (5UL  * 60UL * 1000UL)   /**< CURIOUS      */
#define NB_BOREDOM_LEVEL2_MS         (10UL * 60UL * 1000UL)   /**< SAD + toast  */
#define NB_BOREDOM_LEVEL3_MS         (20UL * 60UL * 1000UL)   /**< SUSPICIOUS   */
#define NB_BOREDOM_LEVEL4_MS         (30UL * 60UL * 1000UL)   /**< ANGRY teatral*/
#define NB_BOREDOM_DEMON_ELIGIBLE_MS (60UL * 60UL * 1000UL)   /**< elegível para modo demônio */

/** Cooldown entre reações comuns (níveis 1–4). */
#define NB_BOREDOM_REACTION_CD_MS    (12UL * 60UL * 1000UL)

/** Cooldown separado para o modo demônio. */
#define NB_BOREDOM_DEMON_CD_MS       (45UL * 60UL * 1000UL)

/** Intervalo do timer de verificação (segundos). */
#define NB_BOREDOM_CHECK_INTERVAL_S  30U

/** Probabilidade do modo demônio por check quando elegível (0–100). */
#define NB_BOREDOM_DEMON_PROB_PCT    5U

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o serviço e cria a task interna.
 *
 * Deve ser chamado após expression_service, led_service, synth_service e
 * ui_overlay_service estarem inicializados. Registra handlers de eventos
 * no event_bus para auto-reset por interação.
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t boredom_service_init(void);

/**
 * @brief Notifica o serviço que ocorreu uma interação.
 *
 * Reinicia o contador de tempo ocioso imediatamente. Thread-safe.
 * Chamado internamente pelos handlers de eventos; também pode ser chamado
 * diretamente por outros serviços.
 */
void boredom_service_on_interaction(void);

/**
 * @brief Pausa ou retoma a escalada de ociosidade.
 *
 * Quando pausado, o timer continua mas nenhuma reação é disparada e o
 * contador não avança. Chamado internamente pelo handler de NB_EVT_STATE_CHANGED.
 *
 * @param paused true = pausar, false = retomar.
 */
void boredom_service_set_paused(bool paused);

/**
 * @brief Pausa por contexto social de presença.
 *
 * Usado por presence_semantic_service via behavior_engine: companhia silenciosa
 * e ausência conhecida não devem virar "abandono" emocional. Thread-safe.
 *
 * @param paused true = pausar por contexto social, false = retomar.
 */
void boredom_service_set_social_paused(bool paused);

#ifdef __cplusplus
}
#endif

#endif /* NB_BOREDOM_SERVICE_H */
