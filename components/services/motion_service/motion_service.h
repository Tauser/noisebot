/*
 * motion_service.h — Serviço de movimento para servos SCS0009
 *
 * Layer 4 do sistema. Responsabilidades:
 *   - Interpolação cossenoidal entre posições.
 *   - API não-bloqueante: comandos enfileirados, executados pela motion_task.
 *   - Parada suave via conclusão do segmento de interpolação atual.
 *   - Sequências: keyframes com hold time entre movimentos.
 *   - Primitivos de pescoço (graus/normalizado) para uso por Layer 5+.
 *
 * Toda posição gerada é validada por motion_safety_check_position() (injetado
 * via motion_safety_iface_t) antes de ir ao servo_hal. Posição rejeitada
 * pela safety layer não é enviada e o movimento é abortado.
 *
 * Conversão de unidades:
 *   SCS0009: 300°/1023 steps → 1° ≈ 3.41 steps. Centro = 512.
 *   Ângulos aqui são *offset* a partir do centro configurado em NVS.
 *   Normalizado [-1, 1] mapeia para [srv_min, srv_max] linearmente.
 *
 * Task: "nb_motion_task"  Core: 1  Prioridade: 20  Stack: 4096
 * A motion_task alimenta o heartbeat da motion_safety a cada iteração.
 *
 * Dependências injetadas em motion_service_init() via motion_safety_iface_t:
 *   - check_position: motion_safety_check_position
 *   - check_velocity: motion_safety_check_velocity
 *   - heartbeat:      motion_safety_heartbeat
 *   - get_min/max/center: config_get_servo_limit_*
 */

#ifndef NB_MOTION_SERVICE_H
#define NB_MOTION_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Interface de safety (injetada no init) ───────────────────────────────── */

/**
 * Ponteiros de função que conectam motion_service à motion_safety e ao
 * config_manager sem criar dependência de compilação em infra.
 */
typedef struct {
    /** Valida posição antes de enviar ao servo. Retorna ESP_OK se aceita. */
    esp_err_t (*check_position)(uint8_t servo_id, uint16_t pos);

    /** Valida velocidade em steps/s. Retorna ESP_OK se aceita. */
    esp_err_t (*check_velocity)(uint8_t servo_id, uint16_t vel_steps_per_s);

    /** Alimenta o watchdog de heartbeat da motion_safety. */
    void (*heartbeat)(void);

    /** Retorna posição mínima do servo em steps (0–1023). */
    int16_t (*get_min)(uint8_t servo_id);

    /** Retorna posição máxima do servo em steps (0–1023). */
    int16_t (*get_max)(uint8_t servo_id);

    /** Retorna posição central do servo em steps (0–1023). */
    int16_t (*get_center)(uint8_t servo_id);
} nb_motion_safety_iface_t;

/* ── Tipos de sequência ───────────────────────────────────────────────────── */

/**
 * Um keyframe de sequência. degrees = offset a partir do centro configurado.
 * Positivo = sentido CW (pan direita / tilt cima — depende da montagem).
 * hold_ms = tempo para aguardar na posição antes do próximo keyframe.
 */
typedef struct {
    uint8_t  servo_id;
    int16_t  degrees;
    uint32_t duration_ms;
    uint32_t hold_ms;
} nb_keyframe_t;

typedef struct {
    const nb_keyframe_t *frames;
    uint8_t              frame_count;
} nb_motion_sequence_t;

/* ── API pública ─────────────────────────────────────────────────────────── */

/**
 * motion_service_init() — Inicializa o serviço com a interface de safety.
 *
 * Deve ser chamado em PHASE_MOTION, antes de motion_service_start().
 * Não cria task — apenas configura estado interno e valida ponteiros.
 *
 * @param iface Ponteiros para funções de safety e config. Não pode ser NULL.
 * @return ESP_OK ou ESP_ERR_INVALID_ARG se iface ou campos obrigatórios NULL.
 */
esp_err_t motion_service_init(const nb_motion_safety_iface_t *iface);

/**
 * motion_service_start() — Cria a motion_task.
 *
 * A task alimenta o heartbeat da safety e executa os comandos enfileirados.
 * Deve ser chamado após motion_safety_arm() ter retornado ESP_OK.
 *
 * @return ESP_OK ou ESP_FAIL se xTaskCreate falhou.
 */
esp_err_t motion_service_start(void);

/**
 * motion_move_to() — Move servo para posição em steps, em duration_ms.
 *
 * Não-bloqueante. Enfileira o comando para execução pela motion_task.
 * A interpolação é cossenoidal: suave na saída e chegada.
 *
 * O comando é REJEITADO (sem enfileirar) se:
 *   - duration_ms == 0
 *   - pos fora dos limites NVS (verificado por check_position)
 *   - fila de comandos cheia
 *
 * @param id          Servo ID.
 * @param pos         Posição alvo em steps (0–1023).
 * @param duration_ms Duração em ms. Mínimo recomendado: 100ms.
 * @return ESP_OK se enfileirado, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM.
 */
esp_err_t motion_move_to(uint8_t id, uint16_t pos, uint32_t duration_ms);

/**
 * motion_stop() — Para o servo suavemente.
 *
 * Congela o servo na posição interpolada atual, encerrando o movimento.
 * Não enfileira — age imediatamente na próxima iteração da motion_task.
 *
 * @param id  Servo ID. 0xFF = todos os servos.
 */
void motion_stop(uint8_t id);

/**
 * motion_park_all() — Move todos os servos para o centro configurado.
 *
 * Usa duration_ms = 500ms para movimento seguro e suave.
 * Não-bloqueante.
 */
esp_err_t motion_park_all(void);

/**
 * motion_sequence_play() — Executa uma sequência de keyframes.
 *
 * Executa os frames em ordem. Frames são movimentos para um único servo;
 * frames diferentes podem ser para servos diferentes (intercalados pela task).
 * Uma sequência em andamento pode ser interrompida por motion_stop() ou
 * por um novo motion_move_to() para o mesmo servo.
 *
 * @param seq  Ponteiro para a sequência. Deve permanecer válido até conclusão.
 * @return     ESP_OK se iniciada, ESP_FAIL se outra sequência está ativa.
 */
esp_err_t motion_sequence_play(const nb_motion_sequence_t *seq);

/* ── Primitivos de pescoço ───────────────────────────────────────────────── */

/**
 * motion_neck_pan() — Pan relativo ao centro, em graus.
 *
 * degrees > 0: pan para direita (sentido CW).
 * degrees < 0: pan para esquerda (sentido CCW).
 * Clampado automaticamente aos limites NVS do servo.
 *
 * @param degrees     Offset a partir do centro (−90..+90, limitado por NVS).
 * @param duration_ms Duração em ms.
 */
esp_err_t motion_neck_pan(int16_t degrees, uint32_t duration_ms);

/**
 * motion_neck_tilt() — Tilt relativo ao centro, em graus.
 *
 * degrees > 0: inclina para cima. degrees < 0: inclina para baixo.
 */
esp_err_t motion_neck_tilt(int16_t degrees, uint32_t duration_ms);

/**
 * motion_neck_look_at() — Posiciona pescoço em coordenadas normalizadas.
 *
 * pan_norm  ∈ [−1, 1]: −1 = limite mínimo pan,  +1 = limite máximo pan.
 * tilt_norm ∈ [−1, 1]: −1 = limite mínimo tilt, +1 = limite máximo tilt.
 * Valores fora de [−1, 1] são clampados.
 *
 * @param pan_norm   Normalizado de pan.
 * @param tilt_norm  Normalizado de tilt.
 * @param duration_ms Duração em ms.
 */
esp_err_t motion_neck_look_at(float pan_norm, float tilt_norm, uint32_t duration_ms);

/**
 * motion_neck_nod() — Aceno com a cabeça (tilt para baixo, depois centro).
 */
esp_err_t motion_neck_nod(void);

/**
 * motion_neck_shake() — Abanar (pan esquerda-direita-centro).
 */
esp_err_t motion_neck_shake(void);

/**
 * motion_neck_tilt_curious() — Inclinação lateral curiosa (pan leve + hold).
 */
esp_err_t motion_neck_tilt_curious(void);

/* ── Getters ─────────────────────────────────────────────────────────────── */

/** Retorna true se o servo id está em movimento. */
bool motion_is_moving(uint8_t id);

/** Retorna posição atual estimada do servo id em steps (último valor escrito). */
uint16_t motion_current_pos(uint8_t id);

#ifdef __cplusplus
}
#endif

#endif /* NB_MOTION_SERVICE_H */
