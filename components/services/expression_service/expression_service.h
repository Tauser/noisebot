/*
 * expression_service.h — Serviço de expressão facial do NoiseBot (modelo EMO)
 *
 * Layer 5 — Core Services.
 * Depende de render_service (Layer 4) e nb_hal/display_hal (Layer 1).
 *
 * Responsabilidades:
 *   - Definir nb_face_state_t (struct paramétrica completa da face).
 *   - Expor 6 expressões base como constantes.
 *   - Interpolar suavemente entre expressões.
 *   - Controlar blink automático com distribuição de Poisson.
 *   - Suporte a blink assimétrico e wink via modelo de olhos independentes.
 *   - Registrar layer de desenho no render_service.
 *
 * Modelo visual EMO:
 *   - Sem pupila. Expressão vem exclusivamente da geometria do shape dos olhos.
 *   - Cada olho é um quadrilátero com cantos independentes, squint e curvatura.
 *   - Boca e sobrancelhas são peças ocasionais (não fazem parte deste modelo).
 *
 * Convenções de parâmetros:
 *   tl/tr      [0..1]  0=neutro, 1=canto superior toca a linha central
 *   bl/br      [0..1]  0=neutro, 1=canto inferior toca a linha central
 *   open       [0..1]  0=fechado, 1=abertura máxima
 *   squint     [0..1]  0=sem descida da pálpebra, 1=pálpebra na linha central
 *   y_l/y_r    [-1..1] offset vertical por olho (+1=baixo, -1=cima)
 *   x_off      [-1..1] -1=olhos afastados, +1=olhos juntos
 *   rt_top     [0..1]  arredondamento dos cantos superiores
 *   rb_bot     [0..1]  arredondamento dos cantos inferiores
 *   cv_top     [-1..1] curvatura da borda superior (+1=convexa, -1=côncava)
 *   cv_bot     [-1..1] curvatura da borda inferior (+1=convexa, -1=côncava)
 *   color               cor do olho (24-bit RGB — LGFX converte para RGB565)
 *
 * transition_ms: parâmetro de expression_service_set(), não campo do estado.
 *
 * Uso típico:
 *   expression_service_init();
 *   expression_service_set(NB_EXPR_HAPPY, 300.0f);
 */

#ifndef NB_EXPRESSION_SERVICE_H
#define NB_EXPRESSION_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Estado paramétrico da face ──────────────────────────────────────────── */

typedef struct {
    /* Olho esquerdo — cantos [0..1] */
    float tl_l;      /**< top-left:     fecha canto sup-esq                   */
    float tr_l;      /**< top-right:    fecha canto sup-dir do olho esq       */
    float bl_l;      /**< bottom-left:  fecha canto inf-esq                   */
    float br_l;      /**< bottom-right: fecha canto inf-dir do olho esq       */

    /* Olho direito — cantos [0..1] */
    float tl_r;
    float tr_r;
    float bl_r;
    float br_r;

    float open_l;    /**< [0..1.5]  abertura vertical olho esq (>1 = arregalado) */
    float open_r;    /**< [0..1.5]  abertura vertical olho dir (>1 = arregalado) */

    float y_l;       /**< [-1..1] offset vertical olho esq (+1=desce)        */
    float y_r;       /**< [-1..1] offset vertical olho dir (+1=desce)        */

    float x_off;     /**< [-1..1] spread horizontal (+1=olhos juntos)        */

    float rt_top;    /**< [0..1]  arredondamento cantos superiores            */
    float rb_bot;    /**< [0..1]  arredondamento cantos inferiores            */

    float cv_top;    /**< [-1..1] curvatura borda superior (+1=convexa)      */
    float cv_bot;    /**< [-1..1] curvatura borda inferior (+1=convexa)      */

    uint32_t color;  /**< cor do olho (24-bit RGB)                            */

    float squint_l;  /**< [0..1]  pálpebra superior olho esq descendo        */
    float squint_r;  /**< [0..1]  pálpebra superior olho dir descendo        */
} nb_face_state_t;

/* ── Expressões base ─────────────────────────────────────────────────────── */

typedef enum {
    NB_EXPR_NEUTRAL    = 0,
    NB_EXPR_HAPPY      = 1,
    NB_EXPR_CURIOUS    = 2,
    NB_EXPR_SLEEPY     = 3,
    NB_EXPR_FOCUSED    = 4,
    NB_EXPR_SUSPICIOUS = 5,
    NB_EXPR_SURPRISED  = 6,
    NB_EXPR_SAD        = 7,
    NB_EXPR_ALARMED    = 8,
    NB_EXPR_COUNT      = 9,
} nb_expression_t;

/** Tabela das expressões base (somente leitura). */
extern const nb_face_state_t NB_EXPRESSIONS[NB_EXPR_COUNT];

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o expression_service.
 *
 * Registra a layer de desenho no render_service.
 * Deve ser chamado após render_service_init() e render_service_start().
 *
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t expression_service_init(void);

/**
 * @brief Define a expressão alvo com transição suave.
 *
 * Interpola linearmente do estado atual para o estado alvo ao longo de
 * transition_ms milissegundos. Thread-safe.
 *
 * @param expr          Expressão alvo (NB_EXPR_NEUTRAL … NB_EXPR_ALARMED).
 * @param transition_ms Duração da transição em ms (0 = imediato).
 */
void expression_service_set(nb_expression_t expr, float transition_ms);

/**
 * @brief Obtém o estado atual interpolado da face.
 *
 * @param[out] out  Estado atual copiado para esta struct.
 */
void expression_service_get_current(nb_face_state_t *out);

/**
 * @brief Interpolação linear entre dois nb_face_state_t.
 *
 * Utilitário público para uso por outros serviços.
 * color faz step em t >= 0.5f.
 *
 * @param a     Estado inicial.
 * @param b     Estado final.
 * @param t     [0.0, 1.0] fração de interpolação.
 * @param[out] out  Resultado.
 */
void nb_face_state_lerp(const nb_face_state_t *a,
                        const nb_face_state_t *b,
                        float t,
                        nb_face_state_t *out);

/**
 * @brief Enfileira uma expressão temporária com retorno automático à base.
 *
 * Após duration_ms, retorna à última expressão definida por expression_service_set.
 * Se um play anterior estiver ativo, este é enfileirado (capacidade: 4 itens).
 * Um novo expression_service_set() cancela o play em curso.
 * Thread-safe.
 *
 * @param expr          Expressão a exibir temporariamente.
 * @param duration_ms   Tempo em ms até retornar à base.
 * @param transition_ms Duração da transição de entrada e saída.
 * @return ESP_OK, ESP_ERR_INVALID_ARG ou ESP_ERR_NO_MEM (fila cheia).
 */
esp_err_t expression_play(nb_expression_t expr,
                          float            duration_ms,
                          float            transition_ms);

/**
 * @brief Frame de uma sequência composta.
 */
typedef struct {
    nb_expression_t expr;
    float           duration_ms;
    float           transition_ms;
} nb_expr_frame_t;

/**
 * @brief Enfileira uma sequência de expressões.
 *
 * Equivalente a chamar expression_play() para cada frame em ordem.
 * Limitado a 4 frames (capacidade da fila interna).
 *
 * @param frames  Array de frames.
 * @param count   Número de frames (máx 4).
 */
void expression_combo_play(const nb_expr_frame_t *frames, uint8_t count);

/**
 * @brief Mostra blush temporário sobre a expressão atual.
 *
 * @param intensity   Intensidade 0..255.
 * @param duration_ms Duração total incluindo fade-out.
 */
void expression_service_overlay_blush(uint8_t intensity, uint32_t duration_ms);

/**
 * @brief Mostra um coração temporário no centro inferior da face.
 *
 * @param duration_ms Duração total incluindo fade-out.
 */
void expression_service_overlay_heart(uint32_t duration_ms);

/**
 * @brief Habilita/desabilita respiração sutil dos olhos em IDLE/ATTENTIVE.
 *
 * O efeito modula a abertura dos olhos no render, sem alterar a expressão base.
 */
void expression_service_set_breath_enabled(bool enabled);

/**
 * @brief Habilita/desabilita o blink automático.
 *
 * Usado por estados persistentes como SLEEPING, onde a expressão base deve
 * permanecer quieta sem piscadas periódicas sobrepostas.
 */
void expression_service_set_blink_enabled(bool enabled);

/**
 * @brief Define o offset de gaze aplicado sobre a expressão atual no render.
 *
 * Aplicado aditivamente no render callback (mesmo frame, após gaze_service):
 *   - Ambos os olhos deslocados na direção x (translation, não convergência).
 *   - y_l e y_r aumentados por y (positivo = olhos descem).
 *
 * Thread-safe somente quando chamado do render_task (Core 1).
 * Deve ser chamado exclusivamente pelo gaze_service render layer (z=5).
 *
 * @param x  [-1, 1]  -1=esquerda, +1=direita
 * @param y  [-1, 1]  -1=cima,    +1=baixo
 */
void expression_service_set_gaze(float x, float y);

#ifdef __cplusplus
}
#endif

#endif /* NB_EXPRESSION_SERVICE_H */
