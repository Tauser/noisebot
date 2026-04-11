#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Estados de energia                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    NB_POWER_UNKNOWN     = 0,   /* Fuel gauge indisponivel           */
    NB_POWER_CHARGING,          /* Carregando via USB/DC/Solar        */
    NB_POWER_DISCHARGING,       /* Descarregando normalmente          */
    NB_POWER_LOW,               /* SoC < LOW_THRESHOLD               */
    NB_POWER_CRITICAL,          /* SoC < CRITICAL_THRESHOLD          */
    NB_POWER_FAULT,             /* Fault no carregador bq25185       */
} nb_power_state_t;

/* ------------------------------------------------------------------ */
/*  Defaults de threshold (configurados via ConfigManager / NVS)      */
/* ------------------------------------------------------------------ */

#define NB_POWER_LOW_PCT_DEFAULT        20
#define NB_POWER_CRITICAL_PCT_DEFAULT   10
#define NB_POWER_SHUTDOWN_MV_DEFAULT    3100

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/*
 * power_manager_init() — Inicializa I2C, MAX17048 e bq25185.
 * Chamado no BOOT_PHASE_2.
 * Em caso de falha no fuel gauge: opera com SoC=NB_POWER_UNKNOWN.
 *
 * TODO (Etapa 0.3): implementar drivers I2C para MAX17048 e bq25185.
 */
esp_err_t power_manager_init(void);

/*
 * power_manager_start() — Inicia PowerMonitorTask (1Hz).
 * Chamado apos event_bus estar pronto.
 */
esp_err_t power_manager_start(void);

nb_power_state_t power_manager_get_state(void);
uint8_t          power_manager_get_soc_pct(void);
uint16_t         power_manager_get_voltage_mv(void);
bool             power_manager_is_charging(void);
bool             power_manager_is_safe_to_start_servos(void);  /* SoC > CRITICAL */
bool             power_manager_is_safe_to_start_camera(void);  /* SoC > LOW      */

#ifdef __cplusplus
}
#endif
