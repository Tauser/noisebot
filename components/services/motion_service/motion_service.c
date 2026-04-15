/*
 * motion_service.c — Serviço de movimento para servos SCS0009
 *
 * Interpolação cossenoidal executada pela motion_task a 50Hz (20ms).
 * Toda posição gerada é validada pela safety interface antes de ir ao HAL.
 *
 * Modelo de threading:
 *   - motion_task: única writer de s_servo[]; lê fila de comandos.
 *   - Callers externos: enfileiram comandos (thread-safe via fila FreeRTOS).
 *   - motion_stop(): escrita atômica de flag, lida na próxima iteração.
 */

#include "motion_service.h"
#include "servo_hal.h"
#include "nb_hw_config.h"

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#define TAG "motion_svc"

/* ── Parâmetros da task ───────────────────────────────────────────────────── */

#define MOTION_TASK_PERIOD_MS   20u     /* 50Hz */
#define MOTION_CMD_QUEUE_DEPTH   8u

/* ── Conversão de unidades ───────────────────────────────────────────────── */
/*
 * SCS0009: 300° de range / 1023 steps.
 * STEPS_PER_DEG = 1023 / 300 ≈ 3.41
 * DEG_PER_STEP  = 300  / 1023 ≈ 0.293°
 */
#define STEPS_PER_DEG_NUM   1023u
#define STEPS_PER_DEG_DEN    300u

/* ── Tipos internos ───────────────────────────────────────────────────────── */

typedef enum {
    CMD_MOVE = 0,
    CMD_STOP,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint8_t    servo_id;
    uint16_t   target_pos;  /* steps, para CMD_MOVE */
    uint32_t   duration_ms;
} motion_cmd_t;

/* Estado de interpolação por servo */
typedef struct {
    uint16_t pos_start;      /* posição no início do move atual        */
    uint16_t pos_target;     /* posição alvo do move atual             */
    uint16_t pos_current;    /* última posição escrita no servo        */
    uint32_t duration_ms;    /* duração total do move                  */
    uint32_t elapsed_ms;     /* ms decorridos no move atual            */
    bool     moving;         /* true enquanto move em andamento        */
    volatile bool stop_req;  /* sinaliza parada na próxima iteração    */
} servo_move_t;

/* Sequência em andamento */
typedef struct {
    const nb_motion_sequence_t *seq;
    uint8_t  frame_idx;
    uint32_t hold_elapsed_ms;
    bool     active;
    bool     holding;        /* aguardando hold_ms após conclusão do frame */
} seq_player_t;

/* ── Estado global ────────────────────────────────────────────────────────── */

static nb_motion_safety_iface_t s_iface;
static servo_move_t             s_servo[2];     /* [0]=PAN [1]=TILT */
static QueueHandle_t            s_cmd_queue = NULL;
static seq_player_t             s_seq = {0};
static bool                     s_initialized = false;
static bool                     s_started = false;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline int servo_index(uint8_t id) {
    return (id == NB_SERVO_ID_PAN) ? 0 : 1;
}

/*
 * cosine_ease() — Curva de easing cossenoidal.
 * t ∈ [0, 1] → saída ∈ [0, 1] com aceleração suave e desaceleração suave.
 * Formula: (1 - cos(π × t)) / 2
 */
static float cosine_ease(float t)
{
    return (1.0f - cosf((float)M_PI * t)) * 0.5f;
}

/*
 * degrees_to_steps() — Converte graus de offset em steps absolutos.
 *
 * @param degrees  Offset a partir do centro (positivo ou negativo).
 * @param servo_id Para obter o centro via iface.
 * @return         Posição em steps, clampada em [get_min, get_max].
 */
static uint16_t degrees_to_steps(int16_t degrees, uint8_t servo_id)
{
    int16_t center = s_iface.get_center(servo_id);
    int16_t min    = s_iface.get_min(servo_id);
    int16_t max    = s_iface.get_max(servo_id);

    /* steps_offset = degrees × (1023 / 300) — inteiro com arredondamento */
    int32_t steps_offset =
        ((int32_t)degrees * (int32_t)STEPS_PER_DEG_NUM + (int32_t)STEPS_PER_DEG_DEN / 2)
        / (int32_t)STEPS_PER_DEG_DEN;

    int32_t pos = (int32_t)center + steps_offset;

    /* Clampa dentro dos limites NVS */
    if (pos < (int32_t)min) pos = (int32_t)min;
    if (pos > (int32_t)max) pos = (int32_t)max;

    return (uint16_t)pos;
}

/*
 * norm_to_steps() — Converte coordenada normalizada [−1, 1] para steps.
 * −1 → get_min, 0 → get_center, +1 → get_max (interpolação linear).
 */
static uint16_t norm_to_steps(float norm, uint8_t servo_id)
{
    int16_t min    = s_iface.get_min(servo_id);
    int16_t max    = s_iface.get_max(servo_id);
    int16_t center = s_iface.get_center(servo_id);

    if (norm < -1.0f) norm = -1.0f;
    if (norm >  1.0f) norm =  1.0f;

    int32_t pos;
    if (norm >= 0.0f) {
        pos = (int32_t)center + (int32_t)((float)(max - center) * norm);
    } else {
        pos = (int32_t)center + (int32_t)((float)(center - min) * norm);
    }

    if (pos < (int32_t)min) pos = (int32_t)min;
    if (pos > (int32_t)max) pos = (int32_t)max;

    return (uint16_t)pos;
}

/*
 * start_move() — Inicia um movimento para um servo.
 * Deve ser chamado apenas de dentro da motion_task.
 */
static void start_move(uint8_t id, uint16_t target_pos, uint32_t duration_ms)
{
    int i = servo_index(id);
    s_servo[i].pos_start   = s_servo[i].pos_current;
    s_servo[i].pos_target  = target_pos;
    s_servo[i].duration_ms = duration_ms;
    s_servo[i].elapsed_ms  = 0;
    s_servo[i].moving      = true;
    s_servo[i].stop_req    = false;
}

/*
 * update_servo() — Atualiza interpolação e escreve posição no hardware.
 * Chamado pela motion_task a cada tick (20ms).
 */
static void update_servo(uint8_t id, uint32_t dt_ms)
{
    int i = servo_index(id);
    servo_move_t *sv = &s_servo[i];

    if (!sv->moving) {
        return;
    }

    /* Stop request: congela na posição atual */
    if (sv->stop_req) {
        sv->stop_req   = false;
        sv->moving     = false;
        /* Mantém pos_current como está — não escreve nova posição */
        ESP_LOGD(TAG, "servo %u: parado em pos=%u", id, sv->pos_current);
        return;
    }

    sv->elapsed_ms += dt_ms;

    float t;
    if (sv->elapsed_ms >= sv->duration_ms) {
        t = 1.0f;
        sv->moving = false;
    } else {
        t = (float)sv->elapsed_ms / (float)sv->duration_ms;
    }

    float ease = cosine_ease(t);
    int32_t delta = (int32_t)sv->pos_target - (int32_t)sv->pos_start;
    uint16_t new_pos = (uint16_t)((int32_t)sv->pos_start + (int32_t)((float)delta * ease));

    /* Valida com safety antes de escrever */
    if (s_iface.check_position(id, new_pos) != ESP_OK) {
        ESP_LOGW(TAG, "servo %u: posição %u rejeitada por safety — abortando move",
                 id, new_pos);
        sv->moving = false;
        return;
    }

    /* Envia ao servo: time_ms = período + pequena folga para tracking suave */
    servo_hal_write_position(id, new_pos, (uint16_t)(MOTION_TASK_PERIOD_MS + 5u));
    sv->pos_current = new_pos;
}

/*
 * advance_sequence() — Avança o player de sequência.
 * Chamado pela motion_task após update_servo, a cada tick.
 */
static void advance_sequence(uint32_t dt_ms)
{
    if (!s_seq.active) {
        return;
    }

    const nb_keyframe_t *frame = &s_seq.seq->frames[s_seq.frame_idx];
    uint8_t id = frame->servo_id;
    int idx = servo_index(id);

    if (s_seq.holding) {
        /* Aguardando hold_ms antes de avançar ao próximo frame */
        s_seq.hold_elapsed_ms += dt_ms;
        if (s_seq.hold_elapsed_ms >= frame->hold_ms) {
            s_seq.frame_idx++;
            if (s_seq.frame_idx >= s_seq.seq->frame_count) {
                s_seq.active = false;
                ESP_LOGD(TAG, "sequência concluída");
                return;
            }
            /* Inicia próximo frame */
            const nb_keyframe_t *next = &s_seq.seq->frames[s_seq.frame_idx];
            uint16_t target = degrees_to_steps(next->degrees, next->servo_id);
            start_move(next->servo_id, target, next->duration_ms);
            s_seq.holding = false;
            s_seq.hold_elapsed_ms = 0;
        }
        return;
    }

    /* Aguarda conclusão do move do servo deste frame */
    if (!s_servo[idx].moving) {
        /* Move concluído — inicia hold */
        if (frame->hold_ms > 0u) {
            s_seq.holding = true;
            s_seq.hold_elapsed_ms = 0;
        } else {
            /* Sem hold: avança imediatamente */
            s_seq.frame_idx++;
            if (s_seq.frame_idx >= s_seq.seq->frame_count) {
                s_seq.active = false;
                return;
            }
            const nb_keyframe_t *next = &s_seq.seq->frames[s_seq.frame_idx];
            uint16_t target = degrees_to_steps(next->degrees, next->servo_id);
            start_move(next->servo_id, target, next->duration_ms);
        }
    }
}

/* ── motion_task ─────────────────────────────────────────────────────────── */
/*
 * Task: "motion_task"  Core: 1  Prioridade: 20  Stack: 4096
 * Roda a 50Hz (20ms). Alimenta heartbeat, processa fila, atualiza servos.
 */
static void motion_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(MOTION_TASK_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();
    motion_cmd_t cmd;

    while (1) {
        /* 1. Alimentar heartbeat da motion_safety */
        s_iface.heartbeat();

        /* 2. Processar comandos da fila (sem bloquear) */
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.type == CMD_STOP) {
                uint8_t stop_id = cmd.servo_id;
                if (stop_id == 0xFFu) {
                    s_servo[0].stop_req = true;
                    s_servo[1].stop_req = true;
                } else {
                    s_servo[servo_index(stop_id)].stop_req = true;
                }
                /* Cancela sequência se ativa */
                if (s_seq.active) {
                    s_seq.active = false;
                    ESP_LOGD(TAG, "sequência interrompida por CMD_STOP");
                }
            } else if (cmd.type == CMD_MOVE) {
                /* Cancela sequência se o move é para um servo da sequência ativa */
                if (s_seq.active) {
                    s_seq.active = false;
                }
                start_move(cmd.servo_id, cmd.target_pos, cmd.duration_ms);
                ESP_LOGD(TAG, "CMD_MOVE servo=%u pos=%u dur=%lums",
                         cmd.servo_id, cmd.target_pos,
                         (unsigned long)cmd.duration_ms);
            }
        }

        /* 3. Avançar sequência (antes de update_servo para detectar conclusão) */
        advance_sequence(MOTION_TASK_PERIOD_MS);

        /* 4. Atualizar interpolação e escrever no hardware */
        update_servo(NB_SERVO_ID_PAN,  MOTION_TASK_PERIOD_MS);
        update_servo(NB_SERVO_ID_TILT, MOTION_TASK_PERIOD_MS);

        vTaskDelayUntil(&last_wake, period);
    }
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t motion_service_init(const nb_motion_safety_iface_t *iface)
{
    if (iface == NULL                   ||
        iface->check_position == NULL   ||
        iface->check_velocity == NULL   ||
        iface->heartbeat      == NULL   ||
        iface->get_min        == NULL   ||
        iface->get_max        == NULL   ||
        iface->get_center     == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_iface, iface, sizeof(s_iface));

    /* Inicializa estado dos servos com posição central */
    uint16_t ctr_pan  = (uint16_t)iface->get_center(NB_SERVO_ID_PAN);
    uint16_t ctr_tilt = (uint16_t)iface->get_center(NB_SERVO_ID_TILT);

    memset(s_servo, 0, sizeof(s_servo));
    s_servo[0].pos_current = ctr_pan;
    s_servo[0].pos_start   = ctr_pan;
    s_servo[0].pos_target  = ctr_pan;
    s_servo[1].pos_current = ctr_tilt;
    s_servo[1].pos_start   = ctr_tilt;
    s_servo[1].pos_target  = ctr_tilt;

    s_cmd_queue = xQueueCreate(MOTION_CMD_QUEUE_DEPTH, sizeof(motion_cmd_t));
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "falha ao criar fila de comandos");
        return ESP_FAIL;
    }

    memset(&s_seq, 0, sizeof(s_seq));
    s_initialized = true;

    ESP_LOGI(TAG, "motion_service inicializado (ctr_pan=%u ctr_tilt=%u)",
             ctr_pan, ctr_tilt);
    return ESP_OK;
}

esp_err_t motion_service_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(
        motion_task, "motion_task",
        4096, NULL,
        20,     /* prioridade: abaixo de safety (23), acima de serviços (3–8) */
        NULL,
        1       /* Core 1 — mesmo que safety_task */
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "falha ao criar motion_task");
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "motion_task criada — servo motion ativo");
    return ESP_OK;
}

esp_err_t motion_move_to(uint8_t id, uint16_t pos, uint32_t duration_ms)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_iface.check_position(id, pos) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    motion_cmd_t cmd = {
        .type        = CMD_MOVE,
        .servo_id    = id,
        .target_pos  = pos,
        .duration_ms = duration_ms,
    };

    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "motion_move_to: fila cheia — comando descartado");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void motion_stop(uint8_t id)
{
    if (!s_started) {
        return;
    }

    motion_cmd_t cmd = {
        .type     = CMD_STOP,
        .servo_id = id,
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
}

esp_err_t motion_park_all(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t ctr_pan  = (uint16_t)s_iface.get_center(NB_SERVO_ID_PAN);
    uint16_t ctr_tilt = (uint16_t)s_iface.get_center(NB_SERVO_ID_TILT);

    esp_err_t r1 = motion_move_to(NB_SERVO_ID_PAN,  ctr_pan,  500u);
    esp_err_t r2 = motion_move_to(NB_SERVO_ID_TILT, ctr_tilt, 500u);

    return (r1 == ESP_OK && r2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t motion_sequence_play(const nb_motion_sequence_t *seq)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (seq == NULL || seq->frames == NULL || seq->frame_count == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_seq.active) {
        return ESP_FAIL;  /* outra sequência em andamento */
    }

    /* Inicia o primeiro frame imediatamente via CMD_MOVE */
    const nb_keyframe_t *first = &seq->frames[0];
    uint16_t target = degrees_to_steps(first->degrees, first->servo_id);
    esp_err_t ret = motion_move_to(first->servo_id, target, first->duration_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    s_seq.seq              = seq;
    s_seq.frame_idx        = 0;
    s_seq.hold_elapsed_ms  = 0;
    s_seq.active           = true;
    s_seq.holding          = false;

    ESP_LOGD(TAG, "sequência iniciada: %u frames", seq->frame_count);
    return ESP_OK;
}

/* ── Primitivos de pescoço ───────────────────────────────────────────────── */

esp_err_t motion_neck_pan(int16_t degrees, uint32_t duration_ms)
{
    uint16_t pos = degrees_to_steps(degrees, NB_SERVO_ID_PAN);
    return motion_move_to(NB_SERVO_ID_PAN, pos, duration_ms);
}

esp_err_t motion_neck_tilt(int16_t degrees, uint32_t duration_ms)
{
    uint16_t pos = degrees_to_steps(degrees, NB_SERVO_ID_TILT);
    return motion_move_to(NB_SERVO_ID_TILT, pos, duration_ms);
}

esp_err_t motion_neck_look_at(float pan_norm, float tilt_norm, uint32_t duration_ms)
{
    uint16_t pan_pos  = norm_to_steps(pan_norm,  NB_SERVO_ID_PAN);
    uint16_t tilt_pos = norm_to_steps(tilt_norm, NB_SERVO_ID_TILT);

    esp_err_t r1 = motion_move_to(NB_SERVO_ID_PAN,  pan_pos,  duration_ms);
    esp_err_t r2 = motion_move_to(NB_SERVO_ID_TILT, tilt_pos, duration_ms);

    return (r1 == ESP_OK && r2 == ESP_OK) ? ESP_OK : r1;
}

/* ── Gestos predefinidos ─────────────────────────────────────────────────── */
/*
 * Os gestos usam motion_sequence_play() com sequências estáticas.
 * Static: os arrays ficam em flash (read-only), sem alocação dinâmica.
 */

esp_err_t motion_neck_nod(void)
{
    static const nb_keyframe_t frames[] = {
        /* tilt para baixo 12°, hold 80ms, sobe 6° acima do centro, volta */
        { NB_SERVO_ID_TILT, -12,  220u, 80u  },
        { NB_SERVO_ID_TILT,   6,  300u, 60u  },
        { NB_SERVO_ID_TILT,   0,  200u,   0u },
    };
    static const nb_motion_sequence_t seq = {
        .frames      = frames,
        .frame_count = (uint8_t)(sizeof(frames) / sizeof(frames[0])),
    };
    return motion_sequence_play(&seq);
}

esp_err_t motion_neck_shake(void)
{
    static const nb_keyframe_t frames[] = {
        { NB_SERVO_ID_PAN, -18, 250u, 60u },
        { NB_SERVO_ID_PAN,  18, 380u, 60u },
        { NB_SERVO_ID_PAN,  -8, 280u, 40u },
        { NB_SERVO_ID_PAN,   0, 200u,  0u },
    };
    static const nb_motion_sequence_t seq = {
        .frames      = frames,
        .frame_count = (uint8_t)(sizeof(frames) / sizeof(frames[0])),
    };
    return motion_sequence_play(&seq);
}

esp_err_t motion_neck_tilt_curious(void)
{
    static const nb_keyframe_t frames[] = {
        { NB_SERVO_ID_PAN,   8, 300u, 800u },  /* leve pan + hold longo */
        { NB_SERVO_ID_TILT, -5, 200u, 600u },  /* leve tilt              */
        { NB_SERVO_ID_TILT,  0, 250u,  0u  },  /* volta centro tilt      */
        { NB_SERVO_ID_PAN,   0, 300u,  0u  },  /* volta centro pan       */
    };
    static const nb_motion_sequence_t seq = {
        .frames      = frames,
        .frame_count = (uint8_t)(sizeof(frames) / sizeof(frames[0])),
    };
    return motion_sequence_play(&seq);
}

/* ── Getters ─────────────────────────────────────────────────────────────── */

bool motion_is_moving(uint8_t id)
{
    return s_servo[servo_index(id)].moving;
}

uint16_t motion_current_pos(uint8_t id)
{
    return s_servo[servo_index(id)].pos_current;
}
