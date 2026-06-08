/*
 * persona_service.h — Personalidade emergente do NoiseBot (Layer 7)
 *
 * Deriva 4 dimensões contínuas [0.0, 1.0] a partir da LTM e persiste em NVS
 * (namespace "nb_persona"). Também mantém um perfil offline-first do usuário
 * atual e o modo de interação do robô com esse usuário. Se o SD falhar no boot,
 * os valores anteriores em NVS são mantidos — a personalidade não regride.
 *
 * Chamadores:
 *   behavior_engine  — conditions de TAP, GREET e VOICE
 *   boot_manager     — init + refresh (a cada boot e após ltm_flush)
 *
 * Fórmulas:
 *   warmth    = 1 − exp(−touch_count / 50)
 *   energy    = clamp(voice_count / hist_count × 6)   — hist_count: ring buffer recente
 *   curiosity = clamp(1 − sleep_ratio × 1.5)
 *   trust     = min(warmth, clamp(sessions / 20))
 */

#ifndef NB_PERSONA_SERVICE_H
#define NB_PERSONA_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NB_USER_ID_MAX_LEN             16U
#define NB_USER_DISPLAY_NAME_MAX_LEN   32U
#define NB_USER_RELATIONSHIP_MAX_LEN   16U
#define NB_USER_LANGUAGE_MAX_LEN       8U
#define NB_ROBOT_NICKNAME_MAX_LEN      24U
#define NB_PERSONA_MODE_MAX_LEN        24U
#define NB_INTERACTION_STYLE_MAX_LEN   24U

typedef struct {
    char user_id[NB_USER_ID_MAX_LEN];
    char display_name[NB_USER_DISPLAY_NAME_MAX_LEN];
    char relationship[NB_USER_RELATIONSHIP_MAX_LEN];
    char language[NB_USER_LANGUAGE_MAX_LEN];
    char robot_nickname[NB_ROBOT_NICKNAME_MAX_LEN];
    char persona_mode[NB_PERSONA_MODE_MAX_LEN];
    char interaction_style[NB_INTERACTION_STYLE_MAX_LEN];
} nb_user_profile_t;

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

/**
 * @brief Retorna o perfil offline-first do usuário atual.
 *
 * Ponteiro para estado interno estático; não modificar. O conteúdo permanece
 * válido até a próxima chamada de setter do persona_service.
 */
const nb_user_profile_t *persona_get_current_user_profile(void);

/**
 * @brief Substitui o perfil offline-first do usuário atual e persiste em NVS.
 *
 * Campos devem ser strings terminadas em NUL e caber nos limites de
 * nb_user_profile_t. Publica NB_EVT_USER_CONTEXT_UPDATED em sucesso.
 */
esp_err_t persona_set_current_user_profile(const nb_user_profile_t *profile);

/**
 * @brief Atualiza somente o modo/persona de interação com o usuário atual.
 */
esp_err_t persona_set_interaction_style(const char *persona_mode,
                                        const char *interaction_style);

#ifdef __cplusplus
}
#endif

#endif /* NB_PERSONA_SERVICE_H */
