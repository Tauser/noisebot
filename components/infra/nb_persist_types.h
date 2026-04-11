#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Versoes de schema — incrementar ao alterar qualquer struct         */
/* ------------------------------------------------------------------ */

#define NB_SCHEMA_EPISODE    1
#define NB_SCHEMA_PREFS      1
#define NB_SCHEMA_PERSONA    1
#define NB_SCHEMA_CONTEXT    1
#define NB_SCHEMA_SNAPSHOT   1

/* ------------------------------------------------------------------ */
/*  Memoria episodica — um registro por evento de interacao            */
/*  Formato no disco: JSONL, append-only, por mes                     */
/*  Path: /noisebot/memory/episodic/YYYY-MM.jsonl                     */
/* ------------------------------------------------------------------ */

typedef enum {
    NB_EP_TOUCH = 0,
    NB_EP_VOICE,
    NB_EP_MOTION,
    NB_EP_SLEEP,
    NB_EP_WAKE,
    NB_EP_ERROR,
    NB_EP_CHARGE,
} nb_episode_type_t;

typedef struct {
    time_t             timestamp;       /* Unix timestamp             */
    nb_episode_type_t  type;
    char               context[16];     /* estado do FSM no momento   */
    char               response[32];    /* acao tomada                */
    uint8_t            soc_pct;         /* bateria no momento         */
    uint8_t            _pad[3];
} nb_episode_t;

/* ------------------------------------------------------------------ */
/*  Preferencias aprendidas do usuario                                 */
/*  Path: /noisebot/memory/semantic/preferences.json                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char     touch_style[16];       /* "gentle", "expressive", ...   */
    char     idle_behavior[16];     /* "calm", "curious", ...        */
    char     preferred_greeting[32];/* nome do asset de audio        */
    uint8_t  voice_sensitivity;     /* 0-100                         */
    uint32_t total_interactions;
    uint32_t avg_touch_duration_ms;
    uint8_t  most_common_trigger;   /* nb_episode_type_t             */
    uint8_t  peak_activity_hour;    /* 0-23                          */
    uint8_t  schema_version;
    uint8_t  _pad;
} nb_preferences_t;

/* ------------------------------------------------------------------ */
/*  Tracas de persona — parametros evolutivos lentos                   */
/*  Path: /noisebot/memory/persona/traits.json                        */
/* ------------------------------------------------------------------ */

typedef struct {
    float    energy_level;          /* 0.0 - 1.0                     */
    float    curiosity;             /* 0.0 - 1.0                     */
    float    expressiveness;        /* 0.0 - 1.0                     */
    float    response_latency_bias; /* 0.0 = rapido, 1.0 = devagar   */
    uint32_t calibrated_interactions;
    uint8_t  schema_version;
    uint8_t  _pad[3];
} nb_persona_traits_t;

/* ------------------------------------------------------------------ */
/*  Contexto entre sessoes                                             */
/*  Path: /noisebot/memory/semantic/context.json                      */
/* ------------------------------------------------------------------ */

typedef struct {
    time_t   last_session_ts;
    uint32_t last_session_duration_s;
    uint8_t  last_trigger;          /* nb_episode_type_t             */
    char     last_response[32];
    uint16_t session_count_today;
    uint8_t  schema_version;
    uint8_t  _pad;
} nb_context_t;

/* ------------------------------------------------------------------ */
/*  Snapshot de estado do sistema                                      */
/*  Path: /noisebot/memory/snapshots/state_current.json               */
/*        /noisebot/memory/snapshots/state_prev.json  (shadow copy)   */
/* ------------------------------------------------------------------ */

typedef struct {
    time_t   timestamp;
    uint32_t boot_count;
    uint32_t uptime_s;
    uint8_t  soc_pct;
    uint16_t voltage_mv;
    bool     is_charging;
    uint32_t free_heap_bytes;
    uint32_t free_psram_bytes;
    char     firmware_version[16];
    uint8_t  active_services;       /* bitmask */
    uint8_t  last_error_code;
    uint8_t  schema_version;
    uint8_t  _pad;
} nb_system_snapshot_t;

#ifdef __cplusplus
}
#endif
