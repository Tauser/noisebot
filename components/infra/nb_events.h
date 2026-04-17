/*
 * nb_events.h — Tipos de eventos do event bus do NoiseBot
 *
 * Define todos os nb_event_type_t do sistema em um único lugar.
 * A implementação do event bus (publish/subscribe) é feita na Etapa 0.5.
 *
 * Convenção de nomenclatura: NB_EVT_<SUBSISTEMA>_<EVENTO>
 */

#ifndef NB_EVENTS_H
#define NB_EVENTS_H

#include <stdint.h>

typedef enum {
    NB_EVT_NONE = 0,

    /* Power (Etapa 0.4) */
    NB_EVT_POWER_BROWNOUT_WARN,     /* brownout detectado — disable motion */
    NB_EVT_POWER_MODE_CHANGED,      /* transição de modo de operação */

    /* Storage (Etapa 0.3) */
    NB_EVT_SD_MOUNTED,              /* SD montado ou re-montado */
    NB_EVT_SD_REMOVED,              /* SD removido durante operação */

    /* Touch (Etapa 2.2) */
    NB_EVT_TOUCH_TAP,
    NB_EVT_TOUCH_LONG_PRESS,
    NB_EVT_TOUCH_SUSTAINED,
    NB_EVT_TOUCH_WAKE,

    /* Voice (Etapa 4.1) */
    NB_EVT_VOICE_ACTIVITY_START,
    NB_EVT_VOICE_ACTIVITY_END,

    /* Audio (Etapa 4.2) */
    NB_EVT_AUDIO_STARTED,
    NB_EVT_AUDIO_ENDED,

    /* Motion (Etapa 3.2) */
    NB_EVT_MOTION_FAULT,            /* safety fault — motion desabilitado */
    NB_EVT_MOTION_ARMED,
    NB_EVT_MOTION_DISABLED,

    /* State machine (Etapa 5.1) */
    NB_EVT_STATE_CHANGED,

    /* Diagnostico (Etapa 9.1) */
    NB_EVT_HEALTH_WARNING,          /* health_score < threshold; data.u32 = score     */
    NB_EVT_HEAP_LOW,                /* PSRAM livre < 300KB;      data.u32 = KB livres */

    /* Behavior (Etapa 9.3) */
    NB_EVT_IDLE_ALONE,              /* sem interação por ALONE_THRESHOLD_MS           */

    /* Sound analysis (Etapa 9.4) */
    NB_EVT_SOUND_CLAP,              /* palmas detectadas (segundo pico em 400ms)      */
    NB_EVT_SOUND_WHISTLE,           /* assobio detectado (narrowband 1-3kHz, >300ms) */
    NB_EVT_SOUND_MUSIC_START,       /* música ambiente detectada (>3s)                */
    NB_EVT_SOUND_MUSIC_END,         /* música parou (>2s de silêncio relativo)        */
    NB_EVT_SOUND_CLASS_CHANGED,     /* transição de classe; data.u32 = nb_sound_class_t */

    NB_EVT_COUNT,                   /* sentinela — manter ao final */
} nb_event_type_t;

#endif /* NB_EVENTS_H */
