#pragma once

#include "nb_persist_types.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Paths no microSD */
#define NB_SD_ROOT              "/sdcard"
#define NB_SD_BASE              NB_SD_ROOT "/noisebot"
#define NB_SD_LOGS              NB_SD_BASE "/logs"
#define NB_SD_ASSETS_AUDIO      NB_SD_BASE "/assets/audio"
#define NB_SD_CONFIG            NB_SD_BASE "/config"
#define NB_SD_MEM_EPISODIC      NB_SD_BASE "/memory/episodic"
#define NB_SD_MEM_SEMANTIC      NB_SD_BASE "/memory/semantic"
#define NB_SD_MEM_PERSONA       NB_SD_BASE "/memory/persona"
#define NB_SD_MEM_SNAPSHOTS     NB_SD_BASE "/memory/snapshots"
#define NB_SD_DIAGNOSTICS       NB_SD_BASE "/diagnostics"
#define NB_SD_HEALTH_FILE       NB_SD_BASE "/.health"

/* Arquivos de memoria */
#define NB_FILE_PREFS           NB_SD_MEM_SEMANTIC  "/preferences.json"
#define NB_FILE_CONTEXT         NB_SD_MEM_SEMANTIC  "/context.json"
#define NB_FILE_PERSONA         NB_SD_MEM_PERSONA   "/traits.json"
#define NB_FILE_SNAP_CURRENT    NB_SD_MEM_SNAPSHOTS "/state_current.json"
#define NB_FILE_SNAP_PREV       NB_SD_MEM_SNAPSHOTS "/state_prev.json"
#define NB_FILE_IMU_CAL         NB_SD_CONFIG        "/imu_calibration.json"

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/*
 * persistence_init() — Monta microSD, cria estrutura de diretorios,
 * verifica health file. Retorna ESP_OK mesmo se SD indisponivel
 * (nesse caso, sd_available() retorna false e operacoes de memoria
 * retornam ESP_ERR_NOT_SUPPORTED silenciosamente).
 * Chamado no BOOT_PHASE_3.
 *
 * TODO (Etapa 1.3b): implementar montagem real do microSD.
 */
esp_err_t persistence_init(void);

bool      persistence_sd_available(void);
bool      persistence_sd_healthy(void);

/*
 * persistence_start() — Inicia SD health monitor task (60s interval).
 */
esp_err_t persistence_start(void);

/* Snapshot de estado do sistema — implementado desde a Etapa 1.3b */
esp_err_t persistence_write_snapshot(const nb_system_snapshot_t *snap);
esp_err_t persistence_load_last_snapshot(nb_system_snapshot_t *snap);

/* Memoria episodica — append-only, uma linha JSONL por episodio */
esp_err_t persistence_record_episode(const nb_episode_t *ep);
esp_err_t persistence_rotate_episodes(uint32_t max_age_days);

/* Preferencias e contexto */
esp_err_t persistence_load_preferences(nb_preferences_t *prefs);
esp_err_t persistence_save_preferences(const nb_preferences_t *prefs);
esp_err_t persistence_load_context(nb_context_t *ctx);
esp_err_t persistence_save_context(const nb_context_t *ctx);

/* Persona */
esp_err_t persistence_load_persona_traits(nb_persona_traits_t *traits);
esp_err_t persistence_save_persona_traits(const nb_persona_traits_t *traits);

#ifdef __cplusplus
}
#endif
