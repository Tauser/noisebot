/*
 * nb_task_config.h — Tabela centralizada de prioridades, stacks e cores das tasks.
 *
 * Regra: toda constante de task FreeRTOS do projeto deve estar aqui.
 * Novas tasks: declarar neste arquivo ANTES de criar.
 * Prioridades 0-24 (FreeRTOS ESP32, configMAX_PRIORITIES=25).
 *
 * Tabela atual (ordem decrescente de prioridade):
 *
 *  Prio | Task             | Core | Stack  | Componente
 *  -----|------------------|------|--------|-------------------
 *   24  | wdog_task        |  0   |  2048  | watchdog_service
 *   23  | safety_task      |  1   |  4096  | motion_safety
 *    9  | nb_main_link     |  0   |  4096  | nb_main_link_service
 *    8  | event_task       |  *   |  4096  | event_bus
 *    7  | nb_render_task   |  1   |  4096  | render_service
 *    7  | nb_push_task     |  1   |  4096  | render_service
 *    6  | audio_task       |  0   |  4096  | audio_service
 *    5  | persistence_task |  0   |  4096  | persistence_mgr
 *    5  | shadow_task      |  0   |  4096  | audio_processor_service
 *    5  | behav_task       |  *   |  4096  | boot_manager
 *    4  | nb_bridge_task   |  0   |  8192  | bridge_service
 *    4  | vision_poll_task |  *   |  8192  | vision_service
 *    2  | opus_worker      |  0   |  4096  | audio_codec_service_v2
 *    2  | codec_worker     |  0   |  4096  | audio_codec_service_v2
 *
 * Nota: dispatcher (prio 8) entrega eventos de safety consumidos por safety_task
 * (prio 23). Eventos de safety críticos podem precisar de entrega direta se a
 * latência do dispatcher for inaceitável — ver F26 da análise 2026-06-11.
 */

#ifndef NB_TASK_CONFIG_H
#define NB_TASK_CONFIG_H

/* ── Watchdog ─────────────────────────────────────────────────────────────── */
#define NB_TASK_WDOG_PRIORITY     24U
#define NB_TASK_WDOG_STACK        2048U
#define NB_TASK_WDOG_CORE         0

/* ── Motion Safety ───────────────────────────────────────────────────────── */
#define NB_TASK_SAFETY_PRIORITY   23U
#define NB_TASK_SAFETY_STACK      4096U
#define NB_TASK_SAFETY_CORE       1

/* ── Inter-MCU link ──────────────────────────────────────────────────────── */
#define NB_TASK_MAIN_LINK_PRIORITY 9U
#define NB_TASK_MAIN_LINK_STACK    4096U
#define NB_TASK_MAIN_LINK_CORE     0

/* ── Event Bus dispatcher ────────────────────────────────────────────────── */
#define NB_TASK_EVENT_PRIORITY    8U
#define NB_TASK_EVENT_STACK       4096U

/* ── Render pipeline ─────────────────────────────────────────────────────── */
#define NB_TASK_RENDER_PRIORITY   7U
#define NB_TASK_RENDER_STACK      4096U
#define NB_TASK_RENDER_CORE       1
#define NB_TASK_PUSH_PRIORITY     7U
#define NB_TASK_PUSH_STACK        3072U
#define NB_TASK_PUSH_CORE         1

/* ── Audio ───────────────────────────────────────────────────────────────── */
#define NB_TASK_AUDIO_PRIORITY    6U
#define NB_TASK_AUDIO_STACK       4096U
#define NB_TASK_AUDIO_CORE        0

/* ── Persistence ─────────────────────────────────────────────────────────── */
#define NB_TASK_PERSIST_PRIORITY  5U
#define NB_TASK_PERSIST_STACK     4096U
#define NB_TASK_PERSIST_CORE      0

/* ── Behavior ────────────────────────────────────────────────────────────── */
#define NB_TASK_BEHAV_PRIORITY    5U
#define NB_TASK_BEHAV_STACK       4096U

/* ── Bridge ──────────────────────────────────────────────────────────────── */
#define NB_TASK_BRIDGE_PRIORITY   4U
#define NB_TASK_BRIDGE_STACK      8192U
#define NB_TASK_BRIDGE_CORE       0

/* ── Vision polling ─────────────────────────────────────────────────────── */
#define NB_TASK_VISION_POLL_PRIORITY 4U
#define NB_TASK_VISION_POLL_STACK    8192U

/* ── Opus/Codec workers ──────────────────────────────────────────────────── */
#define NB_TASK_OPUS_PRIORITY     2U
#define NB_TASK_CODEC_PRIORITY    2U
#define NB_TASK_OPUS_CORE         0
#define NB_TASK_CODEC_CORE        0

#endif /* NB_TASK_CONFIG_H */
