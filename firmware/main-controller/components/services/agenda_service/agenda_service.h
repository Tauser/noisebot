/*
 * agenda_service.h — Timers, lembretes e alarmes locais (Layer 5)
 *
 * 8 slots estáticos por tipo. Sem malloc em nenhum caminho.
 *
 * Timers e lembretes usam base monotônica (remaining_ms decrementado em tick).
 * Alarmes usam hora local + máscara de dias; exigem time_service sincronizado.
 *   Máscara de dias: bit 0 = Domingo, bit 1 = Segunda, ..., bit 6 = Sábado.
 *   Exemplos: 0x7E = Seg–Sáb, 0x3E = Seg–Sex (62 decimal), 0x00 = disparo único.
 * Alarmes são persistidos em NVS (namespace nb_alm) para sobreviver a reboots.
 *
 * Chamar agenda_service_tick(100) a cada 100 ms do behavior_task.
 */

#ifndef NB_AGENDA_SERVICE_H
#define NB_AGENDA_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Capacidade ──────────────────────────────────────────────────────────── */

#define NB_AGENDA_MAX_TIMERS     8U
#define NB_AGENDA_MAX_REMINDERS  8U
#define NB_AGENDA_MAX_ALARMS     8U
#define NB_AGENDA_LABEL_MAX      64U   /* incluindo '\0' */

/* ── Tipos de retorno (cópias snapshot, não ponteiros internos) ──────────── */

typedef struct {
    uint8_t  id;
    bool     active;
    char     label[NB_AGENDA_LABEL_MAX];
    uint32_t duration_ms;
    uint32_t remaining_ms;
} nb_timer_t;

typedef struct {
    uint8_t  id;
    bool     active;
    char     label[NB_AGENDA_LABEL_MAX];
    uint32_t delay_ms;
    uint32_t remaining_ms;
} nb_reminder_t;

typedef struct {
    uint8_t  id;
    bool     active;
    bool     enabled;
    uint8_t  hour;      /* 0–23 */
    uint8_t  minute;    /* 0–59 */
    uint8_t  days;      /* bitmask dias; 0x00 = disparo único */
    char     label[NB_AGENDA_LABEL_MAX];
} nb_alarm_t;

/* ── Init / Tick ─────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o serviço e restaura alarmes do NVS.
 * Deve ser chamado após time_service_init() e após event_bus estar disponível.
 */
esp_err_t agenda_service_init(void);

/**
 * @brief Avança contadores e verifica alarmes.
 * Chamar do behavior_task a cada 100 ms.
 */
void agenda_service_tick(uint32_t dt_ms);

/* ── Timers ──────────────────────────────────────────────────────────────── */

/** @return ESP_ERR_NO_MEM se todos os 8 slots estiverem ocupados. */
esp_err_t agenda_timer_create(uint32_t duration_ms, const char *label,
                              uint8_t *out_id);

/** Reinicia o countdown; retorna ESP_ERR_NOT_FOUND se inativo. */
esp_err_t agenda_timer_update(uint8_t id, uint32_t new_duration_ms,
                              const char *label);

esp_err_t agenda_timer_cancel(uint8_t id);

/** Copia snapshot do slot; retorna true se id válido (mesmo se inativo). */
bool      agenda_timer_get(uint8_t id, nb_timer_t *out);

/* ── Lembretes ───────────────────────────────────────────────────────────── */

esp_err_t agenda_reminder_create(uint32_t delay_ms, const char *label,
                                 uint8_t *out_id);
esp_err_t agenda_reminder_update(uint8_t id, uint32_t new_delay_ms,
                                 const char *label);
esp_err_t agenda_reminder_cancel(uint8_t id);
bool      agenda_reminder_get(uint8_t id, nb_reminder_t *out);

/* ── Alarmes ─────────────────────────────────────────────────────────────── */

esp_err_t agenda_alarm_create(uint8_t hour, uint8_t minute, uint8_t days,
                              const char *label, bool enabled,
                              uint8_t *out_id);
esp_err_t agenda_alarm_update(uint8_t id, uint8_t hour, uint8_t minute,
                              uint8_t days, const char *label);
esp_err_t agenda_alarm_cancel(uint8_t id);
esp_err_t agenda_alarm_set_enabled(uint8_t id, bool enabled);
bool      agenda_alarm_get(uint8_t id, nb_alarm_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_AGENDA_SERVICE_H */
