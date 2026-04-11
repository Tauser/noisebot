#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Tipos de evento                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    /* Sistema */
    EVT_SYSTEM_BOOT_COMPLETE = 0,
    EVT_SYSTEM_SAFE_MODE,
    EVT_SERVICE_STARTED,
    EVT_SERVICE_FAILED,
    EVT_ERROR_REPORTED,

    /* Energia */
    EVT_POWER_CHARGING,
    EVT_POWER_DISCHARGING,
    EVT_POWER_LOW,          /* SoC abaixo de LOW_THRESHOLD  */
    EVT_POWER_CRITICAL,     /* SoC abaixo de CRITICAL_THRESHOLD */
    EVT_POWER_FAULT,        /* Fault no carregador bq25185  */

    /* Storage */
    EVT_STORAGE_DEGRADED,   /* microSD indisponivel ou com falha */
    EVT_STORAGE_RECOVERED,  /* microSD voltou a responder */

    /* Movimento */
    EVT_MOTION_COMMAND,
    EVT_MOTION_COMPLETE,
    EVT_MOTION_FAULT,
    EVT_SERVO_OVERTEMP,
    EVT_SERVO_STALL,

    /* Sensores */
    EVT_TOUCH_START,
    EVT_TOUCH_END,
    EVT_TOUCH_HOLD,
    EVT_IMU_TILT,
    EVT_IMU_FALL,

    /* Audio */
    EVT_AUDIO_CAPTURE_READY,
    EVT_AUDIO_PLAYBACK_START,
    EVT_AUDIO_PLAYBACK_COMPLETE,
    EVT_VOICE_DETECTED,

    /* Comportamento */
    EVT_BEHAVIOR_TRIGGER,
    EVT_BEHAVIOR_COMPLETE,

    NB_EVENT_TYPE_COUNT,
} nb_event_type_t;

/* ------------------------------------------------------------------ */
/*  Payloads por categoria                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  soc_pct;
    uint16_t voltage_mv;
    bool     is_charging;
} nb_evt_power_t;

typedef struct {
    int16_t  pos_target;
    int16_t  pos_current;
    uint8_t  servo_id;
    uint8_t  temperature_c;
    uint8_t  load_pct;
} nb_evt_motion_t;

typedef struct {
    uint32_t duration_ms;
    uint8_t  zone;          /* zona de toque se multiplas zonas */
} nb_evt_touch_t;

typedef struct {
    float ax, ay, az;       /* aceleracao em g */
    float gx, gy, gz;       /* giroscopio em deg/s */
} nb_evt_imu_t;

typedef struct {
    uint16_t energy_rms;    /* energia RMS do frame de audio */
    bool     voice_present;
} nb_evt_audio_t;

typedef struct {
    const char *service_name;
    int         error_code;
} nb_evt_service_t;

/* ------------------------------------------------------------------ */
/*  Evento generico                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    nb_event_type_t type;
    uint32_t        timestamp_ms;   /* ms desde boot (xTaskGetTickCount) */
    const char     *source;         /* TAG do modulo de origem */
    union {
        nb_evt_power_t   power;
        nb_evt_motion_t  motion;
        nb_evt_touch_t   touch;
        nb_evt_imu_t     imu;
        nb_evt_audio_t   audio;
        nb_evt_service_t service;
        uint8_t          raw[16];   /* payload generico para eventos simples */
    } data;
} nb_event_t;

#ifdef __cplusplus
}
#endif
