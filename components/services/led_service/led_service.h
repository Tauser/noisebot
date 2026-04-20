/*
 * led_service.h — Serviço de LED do NoiseBot (Layer 4)
 *
 * Gerencia os 2 LEDs WS2812 com sistema de prioridade em duas camadas:
 *   - base state:    animação persistente (IDLE, BOOT, SAFE_MODE, ERROR)
 *   - overlay:       efeito temporário com retorno automático ao estado base
 *
 * Prioridade de estados (maior vence):
 *   ERROR > SAFE_MODE > TOUCH > BOOT > IDLE
 *
 * Não possui FreeRTOS task própria. Chamar led_service_update(dt_ms) a cada
 * ciclo do loop de controle (recomendado: 20ms / 50Hz). O flush ao HAL é
 * feito apenas quando o frame muda (dirty detection).
 *
 * Threads:
 *   - led_service_update()   chamado de qualquer task única de controle
 *   - demais funções da API  chamáveis de qualquer task (mutex interno)
 *
 * Stack esperado do caller: sem overhead adicional desta API.
 */

#ifndef NB_LED_SERVICE_H
#define NB_LED_SERVICE_H

#include "led_hal.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Paleta nomeada do projeto ───────────────────────────────────────────── */

#define NB_LED_WARM_WHITE    ((nb_led_color_t){255, 200, 120})  /* idle base  */
#define NB_LED_COOL_WHITE    ((nb_led_color_t){255, 255, 220})  /* boot       */
#define NB_LED_ORANGE        ((nb_led_color_t){255, 80,   0 })  /* safe mode  */
#define NB_LED_RED           ((nb_led_color_t){255,  0,   0 })  /* error      */
#define NB_LED_TOUCH_WARM    ((nb_led_color_t){255, 160,  40})  /* touch flash*/
#define NB_LED_BEAT_BLUE     ((nb_led_color_t){ 40, 120, 255})  /* beat flash */
#define NB_LED_AMBER         ((nb_led_color_t){255, 140,  30})  /* meditation */
#define NB_LED_EMBER         ((nb_led_color_t){ 80,  45,  15})  /* silent company (muito baixo) */
#define NB_LED_CYAN_SOFT     ((nb_led_color_t){ 20, 180, 200})  /* attentive/listening          */

/* ── Tipos públicos ──────────────────────────────────────────────────────── */

/**
 * Estados base do sistema — determinam a animação persistente.
 * Prioridade: ERROR(4) > SAFE_MODE(3) > BOOT(2) > IDLE(1).
 * TOUCH(2.5) é tratado como overlay, não base state.
 */
typedef enum {
    NB_LED_BASE_IDLE           = 0,  /**< Breathe quente, brilho baixo            */
    NB_LED_BASE_BOOT           = 1,  /**< Pulso branco — ativo durante boot        */
    NB_LED_BASE_SAFE_MODE      = 2,  /**< Laranja sólido pulsante                 */
    NB_LED_BASE_ERROR          = 3,  /**< Vermelho pulsante rápido                */
    NB_LED_BASE_MEDITATION     = 4,  /**< Âmbar muito lento (6s), meditação       */
    NB_LED_BASE_SILENT_COMPANY = 5,  /**< Brasa quase apagada (8s), companhia     */
    NB_LED_BASE_ATTENTIVE      = 6,  /**< Cyan pulsante médio — escutando         */
    NB_LED_BASE__COUNT,
} nb_led_base_state_t;

/* ── API de inicialização ────────────────────────────────────────────────── */

/**
 * @brief Inicializa o serviço (chama led_hal_init internamente).
 *
 * Deve ser chamado em PHASE_HAL do boot_manager, após infra estar pronta.
 * Estado inicial: NB_LED_BASE_BOOT ativo, overlay limpo.
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t led_service_init(void);

/**
 * @brief Avança todas as animações em dt_ms e faz flush se necessário.
 *
 * Chamar a cada ciclo do loop de controle (ex: 20ms).
 * Não bloqueante — o flush RMT (~400µs) é feito internamente.
 *
 * @param dt_ms Tempo decorrido desde o último update, em milissegundos.
 */
void led_service_update(uint32_t dt_ms);

/* ── Controle de cor direta ──────────────────────────────────────────────── */

/**
 * @brief Define a cor de um LED individual (sem animação).
 *
 * Cancela o overlay se ativo. Não afeta o estado base em curso.
 *
 * @param idx   Índice do LED (0 a NB_LED_COUNT-1).
 * @param color Cor desejada.
 */
void led_set_color(uint8_t idx, nb_led_color_t color);

/**
 * @brief Define a cor de todos os LEDs (sem animação).
 */
void led_set_all(nb_led_color_t color);

/**
 * @brief Ajusta o brilho global (0–255).
 *
 * Aplicado como fator multiplicativo sobre todas as animações.
 * O limitador de corrente interno (NB_LED_MAX_BRIGHTNESS) clipa o valor
 * efetivo antes de enviar ao HAL — não afeta este setter.
 *
 * @param brightness Nível de brilho desejado (0 = apagado, 255 = máximo).
 */
void led_set_brightness(uint8_t brightness);

/* ── Animações como overlay ──────────────────────────────────────────────── */

/**
 * @brief Fade de todos os LEDs para a cor alvo em ms milissegundos.
 *
 * Overlay temporário: ao terminar, retorna ao estado base.
 *
 * @param color Cor alvo.
 * @param ms    Duração do fade em milissegundos (>0).
 */
void led_fade_to(nb_led_color_t color, uint32_t ms);

/**
 * @brief Pisca os LEDs count vezes e retorna ao estado base.
 *
 * Cada blink tem 150ms on + 150ms off. Para blink infinito, usar led_breathe().
 *
 * @param count Número de blinks (1–255). 0 cancela blink em curso.
 */
void led_blink(uint8_t count);

/**
 * @brief Ativa respiração contínua com o período especificado.
 *
 * Não é overlay — substitui a animação do estado base IDLE.
 * Leve diferença de fase entre os 2 LEDs para evitar visual mecânico.
 *
 * @param period_ms Duração de um ciclo completo em ms (recomendado: 2000–6000).
 */
void led_breathe(uint32_t period_ms);

/* ── Presets de estado do sistema ────────────────────────────────────────── */

/**
 * @brief Ativa ou desativa um estado base.
 *
 * O estado de maior prioridade ativo determina a animação atual.
 * Múltiplos estados podem ser marcados como ativos — apenas o de maior
 * prioridade é renderizado.
 *
 * @param state  Estado a modificar.
 * @param active true = ativar, false = desativar.
 */
void led_base_set(nb_led_base_state_t state, bool active);

/**
 * @brief Dispara o efeito de touch (flash_decay quente → base).
 *
 * Overlay de alta prioridade com timeout automático (~600ms).
 * Ignorado se ERROR ou SAFE_MODE estiver ativo.
 */
void led_effect_touch(void);

/**
 * @brief Dispara o efeito heartbeat_pulse (2 batimentos + pausa).
 *
 * Overlay temporário com 1 ciclo completo (~900ms) e retorno ao base.
 */
void led_effect_heartbeat(void);

/**
 * @brief Flash suave sincronizado com beat de ritmo detectado.
 *
 * Overlay curto (~150ms) em azul frio, para pulsar no ritmo da música.
 * Não ativa se ERROR ou SAFE_MODE estiver ativo.
 */
void led_effect_beat(void);

/**
 * @brief Ativa modo noturno (brilho máximo reduzido por fator configurável).
 *
 * @param enable true = modo noturno ativo.
 */
void led_set_night_mode(bool enable);

#endif /* NB_LED_SERVICE_H */
