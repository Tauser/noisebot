/*
 * motion_safety.c — Safety layer para servos SCS0009
 *
 * Implementa monitoramento contínuo de load, temperatura e heartbeat.
 * Em qualquer falha de safety: park_and_disable() antes de mudar estado.
 *
 * Localização em infra/ para evitar dependência circular entre componentes.
 *
 * Task: "nb_safety_task"
 *   Core: 1   Prioridade: 23   Stack: 4096
 *   Período: 50ms (20Hz) — verifica load, temp e heartbeat.
 */

#include "motion_safety.h"
#include "servo_hal.h"
#include "event_bus.h"
#include "nb_events.h"
#include "config_manager.h"
#include "logger.h"
#include "nb_hw_config.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include <stdio.h>

#define TAG "motion_safety"

/* ── Período da safety task ──────────────────────────────────────────────── */

#define SAFETY_TASK_PERIOD_MS   50u     /* 20Hz */

/* ── Estado interno ──────────────────────────────────────────────────────── */

static volatile nb_motion_state_t s_state = NB_MOTION_DISABLED;
static SemaphoreHandle_t          s_mutex = NULL;

/* Heartbeat: timestamp (em ticks) da última chamada a motion_safety_heartbeat().
 * Escrita por qualquer task (motion_task), leitura pela safety_task.
 * Acesso atômico suficiente em 32-bit Xtensa. */
static volatile TickType_t s_heartbeat_tick = 0;
static volatile bool       s_heartbeat_active = false;  /* true após arm() */

/* Rastreamento de overload por servo (índice 0=PAN, 1=TILT) */
typedef struct {
    TickType_t overload_start;  /* tick em que carga >70% começou, 0 se limpo */
    bool       overloading;
} servo_overload_t;

static servo_overload_t s_overload[2];  /* [0]=PAN [1]=TILT */

/* Handle da subscription de brownout */
static nb_event_sub_handle_t s_brownout_sub = NB_EVENT_SUB_INVALID;

static bool s_initialized = false;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static const char *state_name(nb_motion_state_t st)
{
    switch (st) {
        case NB_MOTION_DISABLED:     return "DISABLED";
        case NB_MOTION_INITIALIZING: return "INITIALIZING";
        case NB_MOTION_ARMED:        return "ARMED";
        case NB_MOTION_FAULT:        return "FAULT";
        default:                     return "UNKNOWN";
    }
}

/*
 * park_and_disable() — Envia servos ao centro e desabilita torque.
 *
 * Chamado sempre antes de qualquer transição para DISABLED ou FAULT.
 * Fire-and-forget: não aguarda o servo chegar ao centro — a prioridade é
 * desabilitar o torque rapidamente (requisito: <150ms para stall).
 *
 * Lê centro dos servos via config_manager (defaults: 512).
 */
static void park_and_disable(void)
{
    uint16_t ctr_pan  = (uint16_t)config_get_servo_center(NB_SERVO_ID_PAN);
    uint16_t ctr_tilt = (uint16_t)config_get_servo_center(NB_SERVO_ID_TILT);

    /* Envia posição de parking (400ms) antes de soltar torque */
    servo_hal_write_position(NB_SERVO_ID_PAN,  ctr_pan,  400u);
    servo_hal_write_position(NB_SERVO_ID_TILT, ctr_tilt, 400u);

    /* Desabilita torque imediatamente — servo é liberado */
    servo_hal_disable_torque(NB_SERVO_ID_PAN);
    servo_hal_disable_torque(NB_SERVO_ID_TILT);
}

/*
 * do_fault() — Transição para FAULT com park + eventos.
 *
 * Idempotente: se já estiver em FAULT, não faz nada.
 * Chamado pela safety_task (não pode usar mutex — evita deadlock com arm()).
 */
static void do_fault(const char *reason)
{
    nb_motion_state_t prev = s_state;
    if (prev == NB_MOTION_FAULT) {
        return;
    }

    NB_LOGE(TAG, "FAULT: %s (estado anterior: %s)", reason, state_name(prev));
    s_state = NB_MOTION_FAULT;
    s_heartbeat_active = false;

    park_and_disable();

    nb_event_t evt = { .type = NB_EVT_MOTION_FAULT };
    nb_event_publish_async(&evt);

    NB_LOGW(TAG, "motion_safety: estado FAULT — requer reset para re-armar");
}

/* ── Brownout handler ────────────────────────────────────────────────────── */

static void on_brownout(const nb_event_t *evt, void *ctx)
{
    (void)evt;
    (void)ctx;

    nb_motion_state_t st = s_state;
    if (st == NB_MOTION_DISABLED || st == NB_MOTION_FAULT) {
        return;
    }

    NB_LOGW(TAG, "brownout detectado — desabilitando motion imediatamente");
    s_state = NB_MOTION_DISABLED;
    s_heartbeat_active = false;
    park_and_disable();

    nb_event_t dis_evt = { .type = NB_EVT_MOTION_DISABLED };
    nb_event_publish_async(&dis_evt);
}

/* ── Safety task ─────────────────────────────────────────────────────────── */
/*
 * Roda a 20Hz (50ms). Verifica load, temperatura e heartbeat de cada servo.
 * Só age quando estado == ARMED. Em FAULT: permanece no loop mas não lê.
 *
 * Task: "nb_safety_task"  Core: 1  Prioridade: 23  Stack: 4096
 */
static void safety_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(SAFETY_TASK_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();

    /* Para log de temperatura throttled (evita spam) */
    static uint32_t temp_warn_log_count[2] = {0, 0};
    const uint32_t TEMP_LOG_EVERY = 20u;  /* log a cada 1s (20 × 50ms) */

    while (1) {
        if (s_state == NB_MOTION_ARMED) {

            uint8_t servo_ids[2] = { NB_SERVO_ID_PAN, NB_SERVO_ID_TILT };

            for (int i = 0; i < 2; i++) {
                uint8_t id = servo_ids[i];

                /* ── Leitura de load ──────────────────────────────────────── */
                uint16_t load = 0;
                if (servo_hal_read_load(id, &load) == ESP_OK) {
                    /* Bit 10 é direção; bits 9:0 são magnitude */
                    uint16_t magnitude = load & 0x03FFu;

                    if (magnitude > NB_MOTION_LOAD_FAULT_THRESHOLD) {
                        if (!s_overload[i].overloading) {
                            s_overload[i].overloading    = true;
                            s_overload[i].overload_start = xTaskGetTickCount();
                            NB_LOGW(TAG, "servo %u: carga alta (raw=%u ~%u%%)",
                                    id, magnitude, (magnitude * 100u) / 1023u);
                        } else {
                            TickType_t elapsed_ms =
                                (xTaskGetTickCount() - s_overload[i].overload_start)
                                * portTICK_PERIOD_MS;
                            if (elapsed_ms >= NB_MOTION_LOAD_FAULT_DURATION_MS) {
                                do_fault("stall detectado (load >70% por >100ms)");
                                /* break out of servo loop — fault já aconteceu */
                                break;
                            }
                        }
                    } else if (magnitude > NB_MOTION_LOAD_WARN_THRESHOLD) {
                        s_overload[i].overloading = false;
                        NB_LOGD(TAG, "servo %u: carga moderada (raw=%u ~%u%%)",
                                id, magnitude, (magnitude * 100u) / 1023u);
                    } else {
                        s_overload[i].overloading = false;
                    }
                }

                /* ── Leitura de temperatura ───────────────────────────────── */
                uint8_t temp = 0;
                if (servo_hal_read_temperature(id, &temp) == ESP_OK) {
                    if (temp >= NB_MOTION_TEMP_FAULT_C) {
                        char reason[48];
                        snprintf(reason, sizeof(reason),
                                 "servo %u temperatura critica (%u°C)", id, temp);
                        do_fault(reason);
                        break;
                    } else if (temp >= NB_MOTION_TEMP_WARN_C) {
                        temp_warn_log_count[i]++;
                        if (temp_warn_log_count[i] >= TEMP_LOG_EVERY) {
                            temp_warn_log_count[i] = 0;
                            NB_LOGW(TAG, "servo %u: temperatura alta (%u°C)", id, temp);
                        }
                    } else {
                        temp_warn_log_count[i] = 0;
                    }
                }
            } /* for each servo */

            /* ── Heartbeat check ──────────────────────────────────────────── */
            if (s_heartbeat_active && s_state == NB_MOTION_ARMED) {
                TickType_t elapsed_ms =
                    (xTaskGetTickCount() - s_heartbeat_tick) * portTICK_PERIOD_MS;
                if (elapsed_ms > NB_MOTION_HEARTBEAT_TIMEOUT_MS) {
                    do_fault("heartbeat timeout (motion_task parou de reportar)");
                }
            }

        } /* if ARMED */

        vTaskDelayUntil(&last_wake, period);
    }
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t motion_safety_init(void)
{
    if (s_initialized) {
        NB_LOGW(TAG, "motion_safety já inicializado");
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        NB_LOGE(TAG, "falha ao criar mutex");
        return ESP_FAIL;
    }

    /* Subscreve evento de brownout para disable imediato */
    esp_err_t ret = nb_event_subscribe(NB_EVT_POWER_BROWNOUT_WARN,
                                        on_brownout, NULL,
                                        &s_brownout_sub);
    if (ret != ESP_OK) {
        NB_LOGW(TAG, "nb_event_subscribe(BROWNOUT) falhou: %s — continuando",
                esp_err_to_name(ret));
    }

    /* Cria safety task em Core 1, prioridade máxima de safety */
    BaseType_t rc = xTaskCreatePinnedToCore(
        safety_task, "nb_safety_task",
        4096, NULL,
        23,     /* prioridade: mais alta que qualquer task de aplicação */
        NULL,
        1       /* Core 1 */
    );
    if (rc != pdPASS) {
        NB_LOGE(TAG, "falha ao criar nb_safety_task");
        return ESP_FAIL;
    }

    s_state = NB_MOTION_DISABLED;
    s_initialized = true;

    NB_LOGI(TAG, "motion_safety inicializado — estado: DISABLED");
    return ESP_OK;
}

esp_err_t motion_safety_arm(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    nb_motion_state_t current = s_state;
    if (current == NB_MOTION_FAULT) {
        NB_LOGE(TAG, "arm() recusado: estado FAULT — requer reset do sistema");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (current == NB_MOTION_ARMED) {
        NB_LOGW(TAG, "arm() chamado com sistema já ARMED");
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    s_state = NB_MOTION_INITIALIZING;
    xSemaphoreGive(s_mutex);

    NB_LOGI(TAG, "arm(): iniciando verificação dos servos...");

    /* PING em ambos os servos */
    esp_err_t ret;
    ret = servo_hal_ping(NB_SERVO_ID_PAN);
    if (ret != ESP_OK) {
        NB_LOGE(TAG, "arm(): servo PAN (ID=%d) não respondeu: %s",
                NB_SERVO_ID_PAN, esp_err_to_name(ret));
        s_state = NB_MOTION_DISABLED;
        return ESP_FAIL;
    }

    ret = servo_hal_ping(NB_SERVO_ID_TILT);
    if (ret != ESP_OK) {
        NB_LOGE(TAG, "arm(): servo TILT (ID=%d) não respondeu: %s",
                NB_SERVO_ID_TILT, esp_err_to_name(ret));
        s_state = NB_MOTION_DISABLED;
        return ESP_FAIL;
    }

    /* Sanity check: leitura de posição e temperatura */
    uint16_t pos;
    uint8_t  temp;

    for (int i = 0; i < 2; i++) {
        uint8_t id = (i == 0) ? NB_SERVO_ID_PAN : NB_SERVO_ID_TILT;
        ret = servo_hal_read_position(id, &pos);
        if (ret != ESP_OK) {
            NB_LOGE(TAG, "arm(): falha ao ler posição servo %u: %s",
                    id, esp_err_to_name(ret));
            s_state = NB_MOTION_DISABLED;
            return ESP_FAIL;
        }

        /* Valida posição dentro dos limites configurados */
        int16_t srv_min = config_get_servo_limit_min(id);
        int16_t srv_max = config_get_servo_limit_max(id);
        if ((int16_t)pos < srv_min || (int16_t)pos > srv_max) {
            NB_LOGW(TAG, "arm(): servo %u posição atual %u fora de [%d, %d] — "
                    "provavelmente em posição inicial, prosseguindo",
                    id, pos, srv_min, srv_max);
            /* Não aborta — servo pode estar em posição fora do range de trabalho
             * (ex: foi movido manualmente). arm() ainda é permitido; a validação
             * de limite fica ativa para comandos futuros. */
        } else {
            NB_LOGI(TAG, "arm(): servo %u posição=%u (range=[%d,%d]) OK",
                    id, pos, srv_min, srv_max);
        }

        if (servo_hal_read_temperature(id, &temp) == ESP_OK) {
            if (temp >= NB_MOTION_TEMP_FAULT_C) {
                NB_LOGE(TAG, "arm(): servo %u temperatura crítica (%u°C) — abortando",
                        id, temp);
                s_state = NB_MOTION_DISABLED;
                return ESP_FAIL;
            }
            NB_LOGI(TAG, "arm(): servo %u temperatura=%u°C OK", id, temp);
        }
    }

    /* Zera estado de overload e reinicia heartbeat */
    s_overload[0].overloading = false;
    s_overload[0].overload_start = 0;
    s_overload[1].overloading = false;
    s_overload[1].overload_start = 0;

    s_heartbeat_tick   = xTaskGetTickCount();
    s_heartbeat_active = true;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = NB_MOTION_ARMED;
    xSemaphoreGive(s_mutex);

    NB_LOGI(TAG, "motion_safety: ARMED — monitoramento ativo");

    nb_event_t evt = { .type = NB_EVT_MOTION_ARMED };
    nb_event_publish_async(&evt);

    return ESP_OK;
}

esp_err_t motion_safety_check_position(uint8_t id, uint16_t pos)
{
    if (s_state != NB_MOTION_ARMED) {
        return ESP_ERR_INVALID_STATE;
    }

    int16_t srv_min = config_get_servo_limit_min(id);
    int16_t srv_max = config_get_servo_limit_max(id);

    if ((int16_t)pos < srv_min || (int16_t)pos > srv_max) {
        NB_LOGW(TAG, "check_position: servo %u pos=%u rejeitado [%d, %d]",
                id, pos, srv_min, srv_max);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t motion_safety_check_velocity(uint8_t id, uint16_t vel_steps_per_s)
{
    (void)id;

    if (s_state != NB_MOTION_ARMED) {
        return ESP_ERR_INVALID_STATE;
    }

    if (vel_steps_per_s > NB_MOTION_MAX_VELOCITY_STEPS_PER_S) {
        NB_LOGW(TAG, "check_velocity: vel=%u excede limite=%u steps/s",
                vel_steps_per_s, NB_MOTION_MAX_VELOCITY_STEPS_PER_S);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

void motion_safety_heartbeat(void)
{
    s_heartbeat_tick = xTaskGetTickCount();
}

nb_motion_state_t motion_safety_get_state(void)
{
    return s_state;
}

bool motion_safety_is_armed(void)
{
    return (s_state == NB_MOTION_ARMED);
}

void motion_safety_emergency_stop(void)
{
    if (!s_initialized) {
        return;  /* chamado antes de init — inofensivo */
    }

    nb_motion_state_t st = s_state;
    if (st == NB_MOTION_DISABLED || st == NB_MOTION_FAULT) {
        return;  /* já seguro */
    }

    NB_LOGW(TAG, "emergency_stop: desabilitando motion antes de reiniciar");
    s_state            = NB_MOTION_FAULT;
    s_heartbeat_active = false;
    park_and_disable();
}
