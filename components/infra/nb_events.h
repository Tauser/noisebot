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

    /* Rhythm (Etapa 10.2) */
    NB_EVT_RHYTHM_LOCKED,           /* BPM estabilizou; data.f32 = bpm                */
    NB_EVT_BEAT_TICK,               /* batida detectada enquanto RHYTHM_LOCKED         */
    NB_EVT_RHYTHM_LOST,             /* confiança caiu por > 2s                         */

    /* Touch Semântico (Etapa 10.4) */
    NB_EVT_TOUCH_DOUBLE_TAP,        /* dois taps em < 400ms                           */
    NB_EVT_TOUCH_DEEP,              /* toque sustentado > 8s                          */
    NB_EVT_TOUCH_CARESS,            /* toque sustentado > 15s                         */
    NB_EVT_TOUCH_WARM_PULSE,        /* pulso emocional a cada 1s durante 3-8s         */

    /* Circadiano (Etapa 11.2) */
    NB_EVT_CIRCADIAN_PHASE_CHANGED,  /* data.u32 = nb_circadian_phase_t         */

    /* VAD Semântico (Etapa 10.3) */
    NB_EVT_VOICE_SHORT,             /* interjeição < 500ms                             */
    NB_EVT_VOICE_LONG,              /* discurso > 4s                                   */
    NB_EVT_VOICE_LOUD,              /* RMS médio > 2× baseline durante voz             */
    NB_EVT_VOICE_SOFT,              /* RMS médio < 0.5× baseline durante voz           */
    NB_EVT_VOICE_FOLLOWUP_TIMEOUT,  /* 8-12s de silêncio após última voz               */
    NB_EVT_VOICE_REPEATED,          /* 3 interjeições em < 30s                         */

    /* Modos Especiais (Etapa 11.4) */
    NB_EVT_MILESTONE_TOUCH_50,      /* 50º toque acumulado no LTM                      */
    NB_EVT_MILESTONE_UPTIME_100H,   /* 100 horas de uptime acumulado                   */

    /* WiFi (Etapa 9.6) */
    NB_EVT_WIFI_CONNECTED,          /* associou ao AP (IP ainda pendente)              */
    NB_EVT_WIFI_IP_ACQUIRED,        /* IP obtido via DHCP — serviços web podem iniciar */
    NB_EVT_WIFI_DISCONNECTED,       /* conexão perdida — reconexão em andamento        */

    /* Bridge LLM (Etapa 12.1) */
    NB_EVT_BRIDGE_CONNECTED,        /* bridge conectado; data.u32 = nb_bridge_transport_t */
    NB_EVT_BRIDGE_DISCONNECTED,     /* bridge desconectado (TCP caiu ou UART perdida)     */
    NB_EVT_BRIDGE_SAY,              /* chunk PCM do bridge; data.ptr = nb_bridge_say_chunk_t* */
    NB_EVT_BRIDGE_EXPR,             /* expressão do bridge; data.ptr = nb_bridge_expr_cmd_t*  */
    NB_EVT_BRIDGE_ACTION,           /* ação do bridge; data.u32 = nb_bridge_action_t          */
    NB_EVT_BRIDGE_EMOT_EVENT,       /* evento emocional; data.u32 = nb_emotion_event_t        */
    NB_EVT_BRIDGE_GAZE,             /* gaze override; data.bytes[0..3]=x f32, [4..7]=y f32   */
    NB_EVT_BRIDGE_TEXT_SCROLL,      /* texto para scroll; data.ptr = static string (≤128B)   */
    NB_EVT_BRIDGE_RESPONSE_TIMEOUT, /* bridge não respondeu em 8s após VOICE_END             */

    NB_EVT_COUNT,                   /* sentinela — manter ao final */
} nb_event_type_t;

#endif /* NB_EVENTS_H */
