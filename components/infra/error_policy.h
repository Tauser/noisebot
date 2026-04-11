#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Politica de erro por modulo.
 *
 * Cada modulo declara o comportamento esperado em caso de falha:
 *   NB_ERR_HALT    — falha critica, sistema nao pode continuar
 *   NB_ERR_RETRY   — tentar novamente N vezes antes de degradar
 *   NB_ERR_DEGRADE — continuar sem este modulo, logar aviso
 *
 * Estas macros documentam intencao — o codigo de cada modulo
 * deve seguir a politica declarada aqui.
 */

/* Modulos CRITICOS: falha = HALT imediato */
#define NB_ERR_POLICY_LOGGER           NB_ERR_HALT
#define NB_ERR_POLICY_WATCHDOG         NB_ERR_HALT
#define NB_ERR_POLICY_CONFIG_MANAGER   NB_ERR_HALT
#define NB_ERR_POLICY_EVENT_BUS        NB_ERR_HALT

/* Modulos SEMI-CRITICOS: falha = DEGRADE com log */
#define NB_ERR_POLICY_POWER_MANAGER    NB_ERR_DEGRADE
#define NB_ERR_POLICY_STORAGE          NB_ERR_DEGRADE
#define NB_ERR_POLICY_DISPLAY          NB_ERR_DEGRADE
#define NB_ERR_POLICY_LED              NB_ERR_DEGRADE

/* Modulos NAO-CRITICOS: falha = DEGRADE silencioso + log */
#define NB_ERR_POLICY_IMU              NB_ERR_DEGRADE
#define NB_ERR_POLICY_TOUCH            NB_ERR_DEGRADE
#define NB_ERR_POLICY_SERVO            NB_ERR_DEGRADE
#define NB_ERR_POLICY_AUDIO            NB_ERR_DEGRADE
#define NB_ERR_POLICY_CAMERA           NB_ERR_DEGRADE
#define NB_ERR_POLICY_PERSISTENCE      NB_ERR_DEGRADE

/* Valores das politicas */
typedef enum {
    NB_ERR_HALT    = 0,
    NB_ERR_RETRY   = 1,
    NB_ERR_DEGRADE = 2,
} nb_err_policy_t;

/*
 * Macro auxiliar: loga falha de init de modulo conforme politica.
 * Se politica = HALT, reinicia o sistema.
 */
#define NB_HANDLE_INIT_FAIL(tag, ret, policy)                      \
    do {                                                            \
        if ((ret) != ESP_OK) {                                      \
            ESP_LOGE(tag, "init failed: %s", esp_err_to_name(ret)); \
            if ((policy) == NB_ERR_HALT) {                          \
                ESP_LOGE(tag, "CRITICAL — restarting");             \
                esp_restart();                                      \
            }                                                       \
        }                                                           \
    } while (0)

#ifdef __cplusplus
}
#endif
