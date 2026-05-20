/*
 * led_service.h — Serviço de LED do NoiseBot (Layer 4)
 *
 * Gerencia os 2 LEDs WS2812 com sistema de prioridade em duas camadas:
 *   - base state:    cor/animação persistente (IDLE, MOOD, BOOT, SAFE_MODE)
 *   - overlay:       efeito temporário com retorno automático ao estado base
 *
 * Prioridade de estados (maior vence):
 *   ERROR > SAFE_MODE > SILENT_COMPANY > MEDITATION > RESPONDING >
 *   ATTENTIVE > BOOT > SLEEPING > MOOD > IDLE
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

#ifdef __cplusplus
extern "C" {
#endif

/* ── Paleta nomeada do projeto ───────────────────────────────────────────── */

#define NB_LED_IDLE_CYAN     ((nb_led_color_t){ 35, 210, 255})  /* idle base  */
#define NB_LED_COOL_WHITE    ((nb_led_color_t){255, 255, 220})  /* boot       */
#define NB_LED_ORANGE        ((nb_led_color_t){255, 80,   0 })  /* safe mode  */
#define NB_LED_RED           ((nb_led_color_t){255,  0,   0 })  /* error      */
#define NB_LED_TOUCH_WARM    ((nb_led_color_t){255, 160,  40})  /* touch flash*/
#define NB_LED_BEAT_BLUE     ((nb_led_color_t){ 40, 120, 255})  /* beat flash */
#define NB_LED_AMBER         ((nb_led_color_t){255, 140,  30})  /* meditation */
#define NB_LED_EMBER         ((nb_led_color_t){ 80,  45,  15})  /* silent company (muito baixo) */
#define NB_LED_CYAN_SOFT     ((nb_led_color_t){ 35, 220, 240})  /* attentive/listening          */
#define NB_LED_CYAN_VIVID    ((nb_led_color_t){ 30, 220, 240})  /* curious mood                 */
#define NB_LED_AQUA_HAPPY    ((nb_led_color_t){ 70, 230, 210})  /* happy mood                   */
#define NB_LED_SLEEP_BLUE    ((nb_led_color_t){ 20, 120, 255})  /* sleeping low breathe         */
#define NB_LED_SPEAK_AQUA    ((nb_led_color_t){ 45, 225, 230})  /* responding pulse             */
#define NB_LED_FOCUS_BLUE    ((nb_led_color_t){ 35, 150, 255})  /* focused mood                 */
#define NB_LED_PURPLE_DIM    ((nb_led_color_t){120,  55, 255})  /* suspicious/sad mood          */
#define NB_LED_SURPRISE_SKY  ((nb_led_color_t){ 80, 210, 255})  /* surprised/alarmed mood       */

/* ── Tipos públicos ──────────────────────────────────────────────────────── */

/**
 * Estados base do sistema — determinam a animação persistente.
 * Prioridade: ERROR > SAFE_MODE > SILENT_COMPANY > MEDITATION > RESPONDING >
 * ATTENTIVE > BOOT > SLEEPING > MOOD > IDLE.
 * TOUCH(2.5) é tratado como overlay, não base state.
 */
typedef enum {
    NB_LED_BASE_IDLE           = 0,  /**< Azul/ciano baixo fixo                    */
    NB_LED_BASE_MOOD           = 1,  /**< Cor fixa da expressão emocional atual     */
    NB_LED_BASE_SLEEPING       = 2,  /**< Azul muito baixo, respiração lenta       */
    NB_LED_BASE_BOOT           = 3,  /**< Pulso branco — ativo durante boot        */
    NB_LED_BASE_ATTENTIVE      = 4,  /**< Cyan pulsante médio — escutando          */
    NB_LED_BASE_RESPONDING     = 5,  /**< Pulso suave azul/verde-água — falando    */
    NB_LED_BASE_MEDITATION     = 6,  /**< Âmbar muito lento (6s), meditação        */
    NB_LED_BASE_SILENT_COMPANY = 7,  /**< Brasa quase apagada (8s), companhia      */
    NB_LED_BASE_SAFE_MODE      = 8,  /**< Laranja sólido pulsante                  */
    NB_LED_BASE_ERROR          = 9,  /**< Vermelho pulsante rápido                 */
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
 * Compatibilidade: ajusta o período interno e garante IDLE ativo.
 * O IDLE padrão permanece fixo; respiração persistente hoje pertence ao
 * estado SLEEPING e a estados especiais.
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
 * @brief Define a cor emocional fixa usada acima do IDLE.
 *
 * Quando active=false, remove a cor emocional e volta para o IDLE fixo.
 * Estados de maior prioridade, como SLEEPING/ATTENTIVE/RESPONDING, continuam
 * tendo precedência.
 */
void led_mood_set(nb_led_color_t color, bool active);

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
 * @brief Pulso curto colorido para expressão emocional.
 *
 * Overlay temporário com decaimento automático e retorno ao base.
 */
void led_effect_color_pulse(nb_led_color_t color, float peak, uint32_t duration_ms);

/**
 * @brief Ativa modo noturno (brilho máximo reduzido por fator configurável).
 *
 * @param enable true = modo noturno ativo.
 */
void led_set_night_mode(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* NB_LED_SERVICE_H */
