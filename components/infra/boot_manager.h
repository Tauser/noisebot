#pragma once

#include "esp_err.h"
#include "esp_system.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOISEBOT_VERSION            "0.1.0"
#define NB_RTC_MAGIC                0x4E425354UL    /* "NBST" */
#define NB_SAFE_MODE_CRASH_THRESH   3
#define NB_STABLE_UPTIME_MS         (5UL * 60UL * 1000UL)

/* ------------------------------------------------------------------ */
/*  Fases de boot                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    NB_BOOT_PHASE_0_CORE = 0,  /* WDT, Logger UART, NVS, RTC state  */
    NB_BOOT_PHASE_1_INFRA,     /* ConfigManager, EventBus            */
    NB_BOOT_PHASE_2_POWER,     /* PowerManager (fuel gauge, charger) */
    NB_BOOT_PHASE_3_STORAGE,   /* Display, LEDs, microSD, Persistence*/
    NB_BOOT_PHASE_4_SENSORS,   /* IMU, Touch                         */
    NB_BOOT_PHASE_5_SERVOS,    /* Servo driver + safety layer        */
    NB_BOOT_PHASE_6_AUDIO,     /* Microfone, Speaker                 */
    NB_BOOT_PHASE_7_CAMERA,    /* Camera OV2640                      */
    NB_BOOT_PHASE_8_APP,       /* BehaviorFSM, Persona               */
    NB_BOOT_PHASE_COMPLETE,
} nb_boot_phase_t;

/* ------------------------------------------------------------------ */
/*  Estado persistido em RTC memory                                    */
/*  Sobrevive a brownout e WDT reset. Perdido em power-off.           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t            magic;
    uint32_t            boot_count;
    uint32_t            crash_count;
    esp_reset_reason_t  last_reset;
    bool                brownout_occurred;
    bool                safe_mode_active;
    uint8_t             _pad[2];
} nb_rtc_state_t;

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/*
 * boot_manager_init() — DEVE ser a primeira chamada em app_main().
 * Inicializa WDT, Logger (UART), NVS, RTC state e brownout handler.
 * Em caso de falha critica: reinicia o sistema internamente.
 */
esp_err_t boot_manager_init(void);

/*
 * boot_manager_run() — Executa fases 1-8.
 * Inicia tasks FreeRTOS de cada servico e retorna.
 * Em safe mode, inicia apenas fases 0-2 + display.
 */
esp_err_t boot_manager_run(void);

/*
 * boot_manager_mark_stable() — Deve ser chamado apos NB_STABLE_UPTIME_MS
 * de operacao sem crash. Zera crash_count para prevenir safe mode espurio.
 */
void      boot_manager_mark_stable(void);

bool                boot_manager_is_safe_mode(void);
uint32_t            boot_manager_get_boot_count(void);
uint32_t            boot_manager_get_crash_count(void);
esp_reset_reason_t  boot_manager_get_last_reset(void);
nb_boot_phase_t     boot_manager_get_phase(void);
const char         *boot_manager_reset_reason_str(esp_reset_reason_t r);

#ifdef __cplusplus
}
#endif
