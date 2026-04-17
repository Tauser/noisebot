/*
 * persona_service.h — Personalidade emergente do NoiseBot (Layer 7)
 *
 * Deriva 4 dimensões contínuas [0.0, 1.0] a partir da LTM e persiste em NVS
 * (namespace "nb_persona"). Se o SD falhar no boot, os valores anteriores em
 * NVS são mantidos — a personalidade não regride.
 *
 * Chamadores:
 *   behavior_engine  — conditions de TAP, GREET e VOICE
 *   boot_manager     — init + refresh (a cada boot e após ltm_flush)
 *
 * Fórmulas:
 *   warmth    = 1 − exp(−touch_count / 50)
 *   energy    = clamp(voice_count / sessions × 2)
 *   curiosity = clamp(1 − sleep_ratio × 1.5)
 *   trust     = min(warmth, clamp(sessions / 20))
 */

#ifndef NB_PERSONA_SERVICE_H
#define NB_PERSONA_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o persona_service.
 *
 * Abre NVS e carrega dimensões salvas. Chama persona_service_refresh() se
 * LTM tiver dados válidos (sessions > 0). Deve ser chamado após ltm_init().
 */
esp_err_t persona_service_init(void);

/**
 * @brief Recalcula as 4 dimensões a partir da LTM e persiste em NVS.
 *
 * Chamar após ltm_flush() (a cada 5min e ao entrar em SLEEPING).
 * No-op se não inicializado.
 */
void persona_service_refresh(void);

float persona_get_warmth(void);
float persona_get_energy(void);
float persona_get_curiosity(void);
float persona_get_trust(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_PERSONA_SERVICE_H */
