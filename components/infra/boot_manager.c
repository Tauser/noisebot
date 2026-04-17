/*
 * boot_manager.c — Implementação do gerenciador de boot do NoiseBot
 *
 * Cada fase é implementada como função estática com prefixo phase_.
 * Fases ainda não implementadas retornam ESP_OK imediatamente com log
 * de "stub (etapa X.X)". Isso garante que o boot sequence seja executável
 * do início ao fim mesmo com firmware parcialmente implementado.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"

#include "boot_manager.h"
#include "logger.h"
#include "esp_timer.h"
#include "watchdog_service.h"
#include "error_policy.h"
#include "config_manager.h"
#include "power_monitor.h"
#include "event_bus.h"
#include "sd_hal.h"
#include "persistence_mgr.h"
#include "display_hal.h"
#include "render_service.h"
#include "expression_service.h"
#include "led_service.h"
#include "touch_service.h"
#include "audio_service.h"
#include "servo_hal.h"
#include "motion_safety.h"
#include "motion_service.h"
#include "state_machine.h"
#include "emotion_model.h"
#include "gaze_service.h"
#include "idle_service.h"
#include "conductor.h"
#include "long_term_memory.h"
#include "diagnostics_service.h"
#include "schedule_service.h"
#include "behavior_engine.h"
#include "sound_analysis_service.h"
#include "synth_service.h"
#include "attention_service.h"
#include "rhythm_service.h"
#include "vad_semantic_service.h"
#include "touch_semantic_service.h"
#include "persona_service.h"
#include "nb_hw_config.h"
#include "nb_config_keys.h"

#define TAG "nb_boot"

/* ── Chaves NVS (namespace nb_sys) ──────────────────────────────────────── */

#define NVS_NS_SYS              "nb_sys"
#define NVS_KEY_BOOT_COUNT      "boot_count"
#define NVS_KEY_SAFE_MODE       "safe_mode"
#define NVS_KEY_RESET_REASON    "reset_reason"
#define NVS_KEY_BOOT_SUCCESS    "boot_ok"

/* ── Estado interno ──────────────────────────────────────────────────────── */

static nb_boot_status_t s_status = {
    .current_phase = NB_BOOT_PHASE_NONE,
    .safe_mode     = false,
    .sd_degraded   = false,
    .boot_count    = 0,
    .reset_reason  = 0,
};

static bool s_initialized = false;

/* ── Helpers de NVS (acesso direto, sem config_manager) ──────────────────── */

/*
 * Lê e incrementa boot_count. Define safe_mode se threshold atingido.
 * Salva reset_reason. Retorna ESP_OK mesmo se NVS tiver valores default
 * (primeira inicialização).
 */
static esp_err_t boot_nvs_load_and_update(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_SYS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        NB_LOGE(TAG, "nvs_open(%s) falhou: %s", NVS_NS_SYS, esp_err_to_name(err));
        return err;
    }

    /* Ler boot_count (ignora erro — default 0 se chave não existe). */
    uint32_t count = 0;
    nvs_get_u32(handle, NVS_KEY_BOOT_COUNT, &count);

    /* Incrementar e salvar. */
    count++;
    nvs_set_u32(handle, NVS_KEY_BOOT_COUNT, count);
    s_status.boot_count = count;

    /* Salvar reset reason. */
    uint8_t reason = (uint8_t)esp_reset_reason();
    nvs_set_u8(handle, NVS_KEY_RESET_REASON, reason);
    s_status.reset_reason = reason;

    /* Verificar se deve ativar safe mode. */
    if (count >= NB_BOOT_SAFE_MODE_THRESHOLD) {
        nvs_set_u8(handle, NVS_KEY_SAFE_MODE, 1);
        s_status.safe_mode = true;
    }

    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

/*
 * Reseta boot_count e safe_mode_flag no NVS (chamado em boot_manager_report_success).
 */
static void boot_nvs_clear_fail_count(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS_SYS, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u32(handle, NVS_KEY_BOOT_COUNT, 0);
    nvs_set_u8(handle, NVS_KEY_SAFE_MODE,   0);
    nvs_set_u8(handle, NVS_KEY_BOOT_SUCCESS, 1);
    nvs_set_u8(handle, "brn_count",          0);  /* zera contador de brownouts */
    nvs_commit(handle);
    nvs_close(handle);
}

/* ── Tabela de nomes para log ─────────────────────────────────────────────── */

static const char *phase_name(nb_boot_phase_t phase)
{
    switch (phase) {
        case NB_BOOT_PHASE_EARLY:    return "EARLY";
        case NB_BOOT_PHASE_POWER:    return "POWER";
        case NB_BOOT_PHASE_STORAGE:  return "STORAGE";
        case NB_BOOT_PHASE_HAL:      return "HAL";
        case NB_BOOT_PHASE_SAFETY:   return "SAFETY";
        case NB_BOOT_PHASE_SERVICES: return "SERVICES";
        case NB_BOOT_PHASE_MOTION:   return "MOTION";
        case NB_BOOT_PHASE_COMPLETE: return "COMPLETE";
        default:                     return "UNKNOWN";
    }
}

static const char *reset_reason_name(uint8_t reason)
{
    switch ((esp_reset_reason_t)reason) {
        case ESP_RST_POWERON:  return "POWERON";
        case ESP_RST_EXT:      return "EXT_PIN";
        case ESP_RST_SW:       return "SW_RESET";
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "UNKNOWN";
    }
}

/* ── Transição de fase com log ───────────────────────────────────────────── */

static void phase_enter(nb_boot_phase_t phase)
{
    s_status.current_phase = phase;
    NB_LOGI(TAG, "── FASE %s ──────────────────────────", phase_name(phase));
}

static void phase_ok(nb_boot_phase_t phase)
{
    NB_LOGI(TAG, "FASE %s: OK", phase_name(phase));
}

static void phase_skip(nb_boot_phase_t phase, const char *reason)
{
    NB_LOGW(TAG, "FASE %s: PULADA (%s)", phase_name(phase), reason);
}

/* ── Implementação das fases ─────────────────────────────────────────────── */

/*
 * PHASE_EARLY — Etapas 0.1 e 0.2
 *
 * Inicializa: logger, watchdog, NVS flash, config_manager, reset reason,
 * boot_count e verificação de safe mode.
 * Esta fase é CRÍTICA: falha aqui não pode ser recuperada.
 */
static esp_err_t phase_early(void)
{
    phase_enter(NB_BOOT_PHASE_EARLY);

    /* 1. Logger (primeira coisa — a partir daqui podemos usar NB_LOG*). */
    nb_logger_init(NB_LOG_LEVEL_INFO);

    NB_LOGI(TAG, "NoiseBot firmware v0.1.0 iniciando...");

    /* 2. Registrar a task atual (app_main) no TWDT do boot. */
    esp_err_t err = nb_watchdog_add_task(NULL);
    if (err != ESP_OK) {
        /*
         * TWDT pode não ter sido iniciado pelo sdkconfig se rodando em QEMU
         * ou em ambiente de teste. Log warning e continua — não é fatal para
         * o desenvolvimento em bancada.
         */
        NB_LOGW(TAG, "TWDT add_task falhou (%s) — watchdog desativado para esta task",
                esp_err_to_name(err));
    }

    /* 3. Watchdog service (cria nb_wdog_task). */
    err = nb_watchdog_init();
    NB_ASSERT_FATAL(err == ESP_OK, TAG, "watchdog_init falhou: %s",
                    esp_err_to_name(err));

    /* 4. NVS Flash init. */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        NB_LOGW(TAG, "NVS corrompida ou versao nova — apagando e reiniciando NVS");
        NB_ERROR_CHECK_FATAL(nvs_flash_erase(), TAG, "nvs_flash_erase");
        err = nvs_flash_init();
    }
    NB_ASSERT_FATAL(err == ESP_OK, TAG, "nvs_flash_init falhou: %s",
                    esp_err_to_name(err));

    /* 5. Config manager (Etapa 0.2) — carrega/aplica defaults de configuração. */
    err = config_manager_init();
    NB_ASSERT_FATAL(err == ESP_OK, TAG, "config_manager_init falhou: %s",
                    esp_err_to_name(err));

    /* 6. Ler reset reason, incrementar boot_count, verificar safe mode. */
    err = boot_nvs_load_and_update();
    if (err != ESP_OK) {
        /*
         * Falha em NVS de sistema não impede o boot, mas é preocupante.
         * Continuar sem os contadores — safe mode não será detectado neste boot.
         */
        NB_LOGW(TAG, "boot_nvs_load_and_update falhou: %s — contadores nao atualizados",
                esp_err_to_name(err));
    }

    /* 7. Logar estado do boot. */
    NB_LOGI(TAG, "Reset reason : %s (%u)",
            reset_reason_name(s_status.reset_reason), s_status.reset_reason);
    NB_LOGI(TAG, "Boot count   : %lu (sem sucesso consecutivo)",
            (unsigned long)s_status.boot_count);

    if (s_status.safe_mode) {
        NB_LOGW(TAG, "╔══════════════════════════════════════╗");
        NB_LOGW(TAG, "║         SAFE MODE ATIVO              ║");
        NB_LOGW(TAG, "║  %lu boots sem sucesso consecutivos   ║",
                (unsigned long)s_status.boot_count);
        NB_LOGW(TAG, "║  Motion desabilitado neste boot.     ║");
        NB_LOGW(TAG, "╚══════════════════════════════════════╝");
    }

    /* 8. Event bus — inicializado aqui para que qualquer componente subsequente
     *    (motion_safety em PHASE_SAFETY, power_monitor em PHASE_POWER) possa
     *    chamar nb_event_subscribe() sem crash por mutex NULL. */
    esp_err_t bus_err = nb_event_bus_init();
    NB_ASSERT_FATAL(bus_err == ESP_OK, TAG, "nb_event_bus_init falhou: %s",
                    esp_err_to_name(bus_err));

    /* Alimentar watchdog após operações lentas de NVS. */
    nb_watchdog_feed();

    phase_ok(NB_BOOT_PHASE_EARLY);
    return ESP_OK;
}

/*
 * PHASE_POWER — Etapa 0.4
 *
 * Inicializa: power_monitor (brownout detection, brn_count NVS, callback).
 * Falha é não-fatal — sistema continua sem proteção de brownout.
 * O modo de operação final é definido em PHASE_COMPLETE, após conhecer
 * sd_degraded (PHASE_STORAGE) e safe_mode (PHASE_EARLY).
 */
static esp_err_t phase_power(void)
{
    phase_enter(NB_BOOT_PHASE_POWER);

    esp_err_t err = power_monitor_init();
    NB_ASSERT(err == ESP_OK, TAG, "power_monitor_init falhou: %s — continuando",
              esp_err_to_name(err));

    phase_ok(NB_BOOT_PHASE_POWER);
    return ESP_OK;
}

/*
 * PHASE_STORAGE — Etapa 0.3
 *
 * Inicializa: sd_hal (SPI3 + FATFS) e persistence_mgr (fila + task + log hook).
 * Falha no SD é não-crítica — sistema entra em modo SD-degradado.
 * persistence_mgr é sempre inicializado (fila e task ficam ativas mesmo sem SD).
 */
static esp_err_t phase_storage(void)
{
    phase_enter(NB_BOOT_PHASE_STORAGE);

    /* 1. Tentar montar o SD. Falha não é fatal. */
    esp_err_t err = sd_hal_init();
    if (err != ESP_OK) {
        s_status.sd_degraded = true;
        NB_LOGW(TAG, "SD nao disponivel — modo degradado ativo");
    }

    /* 2. Iniciar persistence_mgr (fila + task + hook de log). */
    err = persistence_mgr_init();
    NB_ASSERT_FATAL(err == ESP_OK, TAG, "persistence_mgr_init falhou: %s",
                    esp_err_to_name(err));

    phase_ok(NB_BOOT_PHASE_STORAGE);
    return ESP_OK;
}

/* ── Task de update do led_service ──────────────────────────────────────── */
/*
 * Tick a 50Hz (20ms). Prioridade baixa — não interfere com render nem safety.
 * Stack: 2KB — led_service_update usa só stack local + mutex.
 *
 * Task: "led_task"  Core: qualquer  Prioridade: 3  Stack: 2048
 */
/* ── Callback de solidão → emotion model ────────────────────────────────── */

static void on_idle_alone(void)
{
    /* Publica NB_EVT_IDLE_ALONE — behavior_engine reage via regra de tabela. */
    nb_event_t evt = { .type = NB_EVT_IDLE_ALONE };
    nb_event_publish(&evt);
}

/* ── Relay de eventos de áudio → event bus ──────────────────────────────── */

/* Relay de áudio: chama SM diretamente (contexto da audio task, não dispatcher)
 * e depois publica no bus para o behavior_engine reagir. */
static void on_audio_event(nb_audio_event_t evt, uint32_t data)
{
    nb_event_t bus_evt = {
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000LL),
        .data.u32     = data,
    };
    switch (evt) {
        case NB_AUDIO_EVT_VOICE_START:
            state_machine_on_voice_start();
            bus_evt.type = NB_EVT_VOICE_ACTIVITY_START;
            break;
        case NB_AUDIO_EVT_VOICE_END:
            state_machine_on_voice_end();
            bus_evt.type = NB_EVT_VOICE_ACTIVITY_END;
            break;
        case NB_AUDIO_EVT_PLAYBACK_START:
            state_machine_on_audio_started();
            bus_evt.type = NB_EVT_AUDIO_STARTED;
            break;
        case NB_AUDIO_EVT_PLAYBACK_END:
            state_machine_on_audio_ended();
            bus_evt.type = NB_EVT_AUDIO_ENDED;
            break;
        default: return;
    }
    nb_event_publish_async(&bus_evt);
}

/* ── Relay de eventos de touch → event bus + state machine + emotion ─────── */

/* Relay de touch: chama SM diretamente (contexto da touch task, não dispatcher)
 * e depois publica no bus para o behavior_engine reagir.
 * led_effect_touch() permanece aqui — é efeito imediato de HAL. */
static void on_touch_event(nb_touch_event_t evt)
{
    /* TAP e SUSTAINED passam pelo touch_semantic_service (Etapa 10.4), que
     * decide se publica TAP simples, DOUBLE_TAP, SUSTAINED, WARM_PULSE, DEEP
     * ou CARESS. LONG_PRESS e WAKE continuam publicados diretamente. */
    switch (evt) {
        case NB_TOUCH_EVT_TAP:
            led_effect_touch();           /* feedback LED imediato — não é comportamento */
            state_machine_on_touch_tap();
            touch_semantic_on_tap();      /* delega publicação ao serviço semântico */
            break;
        case NB_TOUCH_EVT_LONG_PRESS: {
            state_machine_on_touch_long_press();
            nb_event_t e = { .type = NB_EVT_TOUCH_LONG_PRESS };
            nb_event_publish_async(&e);
            break;
        }
        case NB_TOUCH_EVT_SUSTAINED:
            touch_semantic_on_sustained(); /* delega progressão SUSTAINED ao serviço */
            break;
        case NB_TOUCH_EVT_WAKE: {
            state_machine_on_touch_wake();
            nb_event_t e = { .type = NB_EVT_TOUCH_WAKE };
            nb_event_publish_async(&e);
            break;
        }
        default: break;
    }
}

/* ── Task de update do led_service ──────────────────────────────────────── */
static void led_update_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(20);
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        led_service_update(20);
        vTaskDelayUntil(&last_wake, period);
    }
}

/* ── Task de update do touch_service ────────────────────────────────────── */
/*
 * Task: "touch_task"  Core: qualquer  Prioridade: 4  Stack: 2048
 * 50Hz para garantir detecção de TAP em <20ms.
 */
static void touch_update_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(20);
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        touch_service_update(20);
        vTaskDelayUntil(&last_wake, period);
    }
}

/* ── Callbacks de behavior → event bus / NVS ────────────────────────────── */

/*
 * Publicado cada vez que a state machine muda de estado.
 * O payload data.u32 carrega o novo nb_robot_state_t.
 * Também aciona o emotion model para eventos de estado relevantes.
 */
/* Publica NB_EVT_STATE_CHANGED de forma SÍNCRONA.
 * Encoding: bits[7:0] = new_state, bits[15:8] = old_state.
 * Síncrono porque on_state_changed é chamado de dentro de state_machine_on_xxx,
 * que por sua vez é chamado dos callbacks de HAL (audio/touch tasks) — nunca
 * do dispatcher. Usar publish_async daqui causaria re-entrância se o dispatcher
 * chamasse SM inputs (o que não faz mais, mas publish sync é mais seguro). */
static void on_state_changed(nb_robot_state_t new_state,
                              nb_robot_state_t old_state,
                              const char *reason)
{
    (void)reason;
    nb_event_t evt = {
        .type         = NB_EVT_STATE_CHANGED,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000LL),
        .data.u32     = ((uint32_t)old_state << 8) | (uint32_t)new_state,
    };
    nb_event_publish(&evt);
}

/* motion_safety publica NB_EVT_MOTION_FAULT diretamente no bus (Layer 3).
 * Este subscriber chama SM para registrar o fault. Chamado pelo dispatcher —
 * state_machine_on_motion_fault() não publica eventos, então é seguro. */
static void on_motion_fault_event(const nb_event_t *evt, void *ctx)
{
    (void)evt; (void)ctx;
    state_machine_on_motion_fault();
}

/* VOICE_FOLLOWUP_TIMEOUT: gaze sweep lateral — "cadê você?" */
static void on_voice_followup_timeout(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    /* Olha para um lado aleatório: simula busca visual da fonte do som */
    float sign = (esp_random() & 1U) ? 1.0f : -1.0f;
    gaze_service_set_target(sign * 0.55f, 0.05f);
}

/* VOICE_SOFT: gaze inclina levemente para cima (postura curiosa) */
static void on_voice_soft(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    gaze_service_set_target(0.0f, -0.15f);
}



/*
 * Persistir a emoção no NVS sempre que a expressão mapeada mudar.
 */
static void on_emotion_changed(nb_expression_t new_expr)
{
    config_set_last_emotion((uint8_t)new_expr);
}

/* ── Task de behavior (10 Hz) ───────────────────────────────────────────── */
/*
 * Tick a 100ms. Avança timers da state machine (idle timeout, touch timeout)
 * e o decaimento emocional.
 *
 * Task: "behav_task"  Core: qualquer  Prioridade: 5  Stack: 4096
 * Stack 4096: expression_service_set (C++) usa frame considerável.
 */
static void behavior_task(void *arg)
{
    (void)arg;
    const TickType_t period        = pdMS_TO_TICKS(100);
    TickType_t       last_wake     = xTaskGetTickCount();
    uint32_t         stats_tick    = 0;
    uint32_t         ltm_tick_n    = 0;
    /* Curiosidade espontânea em IDLE: 25–60s, dispara NB_ACTION_CURIOUS. */
    uint32_t         curious_ms    = 25000u + (esp_random() % 35000u);

    while (1) {
        state_machine_update(100);
        /* energy > 0.7: transições emocionais 30% mais rápidas (persona 11.1) */
        emotion_model_update(persona_get_energy() > 0.7f ? 130u : 100u);
        attention_service_tick(100);
        rhythm_service_tick(100);
        vad_semantic_tick(100);
        touch_semantic_tick(100);
        idle_service_update(100);
        ltm_tick(100);

        /* Curiosidade espontânea: robô olha de forma curiosa ocasionalmente em IDLE. */
        {
            nb_robot_state_t st = state_machine_get_state();
            if (st == NB_STATE_IDLE) {
                if (curious_ms <= 100u) {
                    conductor_play(NB_ACTION_CURIOUS);
                    curious_ms = 25000u + (esp_random() % 35000u);
                } else {
                    curious_ms -= 100u;
                }
            } else {
                /* Fora de IDLE: reseta timer para não disparar logo ao retornar. */
                curious_ms = 15000u + (esp_random() % 20000u);
            }
        }

        /* Diagnostics a cada 60s (600 x 100ms). */
        if (++stats_tick >= 600u) {
            diagnostics_collect();
            stats_tick = 0;
        }

        /* LTM flush a cada 5min (3000 × 100ms) + refresh de persona. */
        if (++ltm_tick_n >= 3000u) {
            ltm_flush();
            persona_service_refresh();
            idle_service_set_saccade_multiplier(
                persona_get_curiosity() > 0.6f ? 1.5f : 1.0f);
            ltm_tick_n = 0;
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

/*
 * PHASE_HAL — Etapa 1.1 + Blocos 2-3
 *
 * Inicializa: display_hal (Etapa 1.1), led_service (Etapa 2.1).
 * Touch (Etapa 2.2), Servo PING (Etapa 3.1): stubs.
 * Falha no display é não-crítica em safe mode.
 */
static esp_err_t phase_hal(void)
{
    phase_enter(NB_BOOT_PHASE_HAL);

    /* Display (Etapa 1.1) */
    esp_err_t err = display_hal_init();
    if (err != ESP_OK) {
        if (s_status.safe_mode) {
            NB_LOGW(TAG, "display_hal_init falhou em safe mode: %s — continuando sem display",
                    esp_err_to_name(err));
        } else {
            NB_ASSERT_FATAL(false, TAG, "display_hal_init falhou: %s", esp_err_to_name(err));
        }
    }

    /* LEDs (Etapa 2.1) */
    err = led_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "led_service_init falhou: %s — LEDs desativados", esp_err_to_name(err));
    } else {
        BaseType_t rc = xTaskCreate(led_update_task, "led_task",
                                    2048, NULL, 3, NULL);
        NB_ASSERT(rc == pdPASS, TAG, "xTaskCreate led_task falhou");
    }

    /* Touch (Etapa 2.2) */
    {
        /*
         * Threshold = baseline × (1 + sens_factor).
         *
         * Medições no hardware (GPIO2, pad de cobre):
         *   baseline calibrado: ~72500–73000
         *   idle real (drift):  ~73400–73500  (+1.3% acima do baseline)
         *   toque forte:        ~300000–350000 (+340% acima do baseline)
         *
         * sens_factor = 0.5f → threshold = baseline × 1.5 ≈ 109000
         *   - idle (~73490) fica bem abaixo → sem falsos disparos
         *   - toque (~320000) fica muito acima → detecção confiável
         *
         * Referência: NodeBot (mesmo hardware) usa mean × 150% com sucesso.
         * O drift de 1.3% não afeta com threshold de 50%.
         */
        float sens_factor = 0.5f;

        err = touch_service_init();
        if (err != ESP_OK) {
            NB_LOGW(TAG, "touch_service_init falhou: %s — touch desativado",
                    esp_err_to_name(err));
        } else {
            touch_service_set_sensitivity(sens_factor);
            touch_service_set_event_cb(on_touch_event);
            BaseType_t rc = xTaskCreate(touch_update_task, "touch_task",
                                        4096, NULL, 4, NULL);
            NB_ASSERT(rc == pdPASS, TAG, "xTaskCreate touch_task falhou");
        }
    }

    /* Servo HAL — inicializa UART apenas. PINGs e verificação acontecem
     * em motion_safety_arm() durante PHASE_MOTION. Sem servos conectados:
     * init UART é inofensivo, arm() falha silenciosamente e o sistema
     * continua operando normalmente sem motion. */
    if (!s_status.safe_mode) {
        err = servo_hal_init();
        if (err != ESP_OK) {
            NB_LOGW(TAG, "servo_hal_init falhou: %s — servos desativados",
                    esp_err_to_name(err));
        }
    }

    phase_ok(NB_BOOT_PHASE_HAL);
    return ESP_OK;
}

/*
 * PHASE_SAFETY — Etapa 3.2
 *
 * Inicializa motion_safety (mutex, brownout sub, safety_task).
 * Não arma os servos aqui — arm() acontece em PHASE_MOTION (Etapa 3.3).
 * Em safe mode: pulada inteira.
 */
static esp_err_t phase_safety(void)
{
    phase_enter(NB_BOOT_PHASE_SAFETY);

    if (s_status.safe_mode) {
        phase_skip(NB_BOOT_PHASE_SAFETY, "safe mode ativo");
        return ESP_OK;
    }

    esp_err_t err = motion_safety_init();
    if (err != ESP_OK) {
        NB_LOGE(TAG, "motion_safety_init falhou: %s", esp_err_to_name(err));
        /* Falha no safety init é fatal — não podemos operar sem safety */
        return err;
    }

    phase_ok(NB_BOOT_PHASE_SAFETY);
    return ESP_OK;
}

/*
 * PHASE_SERVICES — Etapa 0.5 + Blocos 4-5
 *
 * Inicializa: event bus (Etapa 0.5). Serviços de domínio (render, audio,
 * behavior, conductor) são stubs aguardando implementação nos Blocos 4-5.
 * Em safe mode: event bus é inicializado mesmo assim (necessário para logs
 * de safety); serviços de domínio são pulados.
 */
static esp_err_t phase_services(void)
{
    phase_enter(NB_BOOT_PHASE_SERVICES);

    /* Event bus já inicializado em PHASE_EARLY — disponível desde o boot. */
    esp_err_t err;

    if (s_status.safe_mode) {
        phase_skip(NB_BOOT_PHASE_SERVICES, "safe mode ativo — servicos de dominio pulados");
        return ESP_OK;
    }

    /* render_service (Etapa 1.2): framebuffer + render_task. */
    err = render_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "render_service_init falhou: %s — display sem render_task",
                esp_err_to_name(err));
    } else {
        err = render_service_start();
        NB_ASSERT(err == ESP_OK, TAG, "render_service_start falhou: %s",
                  esp_err_to_name(err));
    }

    /* expression_service (Etapa 1.3): face procedural + blink */
    err = expression_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "expression_service_init falhou: %s",
              esp_err_to_name(err));

    /* audio_service (Bloco 4) */
    err = audio_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "audio_service_init falhou: %s — audio desativado",
                esp_err_to_name(err));
    } else {
        audio_service_set_event_cb(on_audio_event);
        audio_set_volume(config_get_volume());
        synth_set_volume(config_get_volume());
    }

    /* sound_analysis_service (Etapa 9.4): FFT 256pt + classificação */
    err = sound_analysis_init();
    NB_ASSERT(err == ESP_OK, TAG, "sound_analysis_init falhou: %s",
              esp_err_to_name(err));

    /* synth_service (Etapa 9.5): síntese procedural de áudio */
    err = synth_init();
    NB_ASSERT(err == ESP_OK, TAG, "synth_init falhou: %s",
              esp_err_to_name(err));

    /* attention_service (Etapa 10.1): modelo contínuo de atenção */
    err = attention_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "attention_service_init falhou: %s",
              esp_err_to_name(err));

    /* rhythm_service (Etapa 10.2): detecção de BPM por autocorrelação */
    err = rhythm_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "rhythm_service_init falhou: %s",
              esp_err_to_name(err));


    /* vad_semantic_service (Etapa 10.3): análise semântica de sessões de voz */
    err = vad_semantic_init();
    NB_ASSERT(err == ESP_OK, TAG, "vad_semantic_init falhou: %s",
              esp_err_to_name(err));
    nb_event_subscribe(NB_EVT_VOICE_FOLLOWUP_TIMEOUT, on_voice_followup_timeout, NULL, NULL);
    nb_event_subscribe(NB_EVT_VOICE_SOFT,             on_voice_soft,             NULL, NULL);

    /* touch_semantic_service (Etapa 10.4): double-tap, DEEP, CARESS, WARM_PULSE */
    err = touch_semantic_init();
    NB_ASSERT(err == ESP_OK, TAG, "touch_semantic_init falhou: %s",
              esp_err_to_name(err));

    /* state_machine e emotion_model (Etapa 5.1) */
    err = state_machine_init(config_get_idle_timeout_s(),
                              s_status.safe_mode,
                              on_state_changed);
    NB_ASSERT(err == ESP_OK, TAG, "state_machine_init falhou: %s",
              esp_err_to_name(err));

    err = emotion_model_init((nb_expression_t)config_get_last_emotion(),
                              on_emotion_changed);
    NB_ASSERT(err == ESP_OK, TAG, "emotion_model_init falhou: %s",
              esp_err_to_name(err));

    /* behavior_task: ticks a 10Hz para idle timeout e decaimento emocional.
     * Stack 4096: emotion_model_update → expression_service_set (C++) usa frame
     * considerável — 2048 causa overflow. */
    {
        BaseType_t rc = xTaskCreate(behavior_task, "behav_task",
                                    4096, NULL, 5, NULL);
        NB_ASSERT(rc == pdPASS, TAG, "xTaskCreate behav_task falhou");
    }

    /* gaze_service (Etapa 5.2): render layer z=5, saccade + micro-drift */
    err = gaze_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "gaze_service_init falhou: %s",
              esp_err_to_name(err));

    /* idle_service (Etapa 5.2): micro-saccades, aversive gaze, yawn */
    err = idle_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "idle_service_init falhou: %s",
              esp_err_to_name(err));
    idle_service_set_alone_cb(on_idle_alone);

    /* conductor (Etapa 5.4): orquestrador de ações de alto nível */
    err = conductor_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "conductor_init falhou: %s — acoes coordenadas desativadas",
                esp_err_to_name(err));
    }

    /* long_term_memory (Etapa 6.2): persona_state + histórico de interações */
    err = ltm_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "ltm_init falhou: %s — LTM desativada",
                esp_err_to_name(err));
    }

    /* persona_service (Etapa 11.1): dimensões de personalidade emergente */
    err = persona_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "persona_service_init falhou: %s — persona em defaults",
                esp_err_to_name(err));
    }
    idle_service_set_saccade_multiplier(
        persona_get_curiosity() > 0.6f ? 1.5f : 1.0f);

    /* behavior_engine (Etapa 9.3): tabela de regras — subscreve ao event bus */
    err = behavior_engine_init();
    NB_ASSERT(err == ESP_OK, TAG, "behavior_engine_init falhou: %s",
              esp_err_to_name(err));

    /* motion_safety fault → SM: subscriber direto (não via behavior_engine,
     * pois state_machine_on_motion_fault é SM input, não comportamento). */
    nb_event_subscribe(NB_EVT_MOTION_FAULT, on_motion_fault_event, NULL, NULL);

    /* schedule_service (Etapa 9.2): timers one-shot e recorrentes sem task */
    err = schedule_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "schedule_service_init falhou: %s",
              esp_err_to_name(err));

    /* diagnostics_service (Etapa 9.1): observabilidade e health score */
    {
        const nb_diagnostics_config_t diag_cfg = {
            .get_fps = render_service_get_fps,
        };
        err = diagnostics_init(&diag_cfg);
        NB_ASSERT(err == ESP_OK, TAG, "diagnostics_init falhou: %s",
                  esp_err_to_name(err));
    }

    phase_ok(NB_BOOT_PHASE_SERVICES);
    return ESP_OK;
}

/* Interface de safety injetada no motion_service */
static const nb_motion_safety_iface_t k_motion_safety_iface = {
    .check_position = motion_safety_check_position,
    .check_velocity = motion_safety_check_velocity,
    .heartbeat      = motion_safety_heartbeat,
    .get_min        = config_get_servo_limit_min,
    .get_max        = config_get_servo_limit_max,
    .get_center     = config_get_servo_center,
};

/*
 * PHASE_MOTION — Etapa 3.3
 *
 * 1. Inicializa motion_service com interface de safety.
 * 2. Arma os servos via motion_safety_arm().
 * 3. Inicia motion_task (que passa a alimentar o heartbeat).
 * 4. Parca todos os servos no centro (posição inicial segura).
 *
 * Falha no arm é não-fatal — robot opera sem movimento.
 * Em safe mode: pulada.
 */
static esp_err_t phase_motion(void)
{
    phase_enter(NB_BOOT_PHASE_MOTION);

    if (s_status.safe_mode) {
        phase_skip(NB_BOOT_PHASE_MOTION, "safe mode ativo");
        return ESP_OK;
    }

    /* 1. Inicializa motion_service */
    esp_err_t err = motion_service_init(&k_motion_safety_iface);
    if (err != ESP_OK) {
        NB_LOGW(TAG, "motion_service_init falhou: %s — robot sem movimento",
                esp_err_to_name(err));
        return ESP_OK;
    }

    /* 2. Arma os servos */
    err = motion_safety_arm();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "motion_safety_arm falhou: %s — robot sem movimento",
                esp_err_to_name(err));
        return ESP_OK;
    }

    /* 3. Inicia motion_task (alimenta heartbeat a partir daqui) */
    err = motion_service_start();
    if (err != ESP_OK) {
        NB_LOGE(TAG, "motion_service_start falhou: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    /* 4. Parking inicial: servos vão para o centro */
    motion_park_all();

    phase_ok(NB_BOOT_PHASE_MOTION);
    return ESP_OK;
}

/*
 * PHASE_COMPLETE — boot concluído.
 *
 * Define modo de operação final baseado no estado acumulado do boot.
 * Prioridade: SAFE_MODE > SD_DEGRADED > NORMAL.
 */
static esp_err_t phase_complete(void)
{
    phase_enter(NB_BOOT_PHASE_COMPLETE);

    /* Definir modo de operação com base no estado do boot. */
    if (s_status.safe_mode) {
        power_monitor_set_mode(NB_POWER_SAFE_MODE, "safe_mode ativo no boot");
        NB_LOGW(TAG, "Boot completo em SAFE MODE — motion desabilitado");
    } else if (s_status.sd_degraded) {
        power_monitor_set_mode(NB_POWER_SD_DEGRADED, "SD nao disponivel");
        NB_LOGW(TAG, "Boot completo — modo SD_DEGRADED (logging so UART)");
    } else {
        power_monitor_set_mode(NB_POWER_NORMAL, "boot bem-sucedido");
        NB_LOGI(TAG, "Boot completo — sistema operacional");
    }

    /* Reportar sucesso (reseta boot_count e brn_count no NVS). */
    boot_manager_report_success();

    /* State machine: BOOT_UP → IDLE (boot concluído). */
    state_machine_on_boot_complete();

    /* Transição de LED: BOOT → IDLE (breathe quente). */
    led_base_set(NB_LED_BASE_BOOT, false);
    led_base_set(NB_LED_BASE_IDLE, true);

    /* Saudação de boot: coordena face + motion + áudio como primeira expressão. */
    if (!s_status.safe_mode) {
        conductor_play(NB_ACTION_GREET);
    }

    phase_ok(NB_BOOT_PHASE_COMPLETE);
    return ESP_OK;
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t boot_manager_run(void)
{
    NB_ASSERT_FATAL(!s_initialized, TAG, "boot_manager_run chamado mais de uma vez");
    s_initialized = true;

    /*
     * Fases críticas (EARLY): falha → NB_ASSERT_FATAL dentro da fase.
     * Fases não-críticas: falha → log + continua em modo degradado.
     *
     * A verificação de esp_err_t aqui é para fases que retornam erro
     * de forma recuperável (ex: STORAGE com SD ausente).
     */

    esp_err_t err;

    err = phase_early();
    NB_ASSERT_FATAL(err == ESP_OK, TAG, "PHASE_EARLY falhou: %s", esp_err_to_name(err));

    err = phase_power();
    NB_ASSERT(err == ESP_OK, TAG, "PHASE_POWER falhou: %s — continuando", esp_err_to_name(err));

    err = phase_storage();
    NB_ASSERT(err == ESP_OK, TAG, "PHASE_STORAGE falhou: %s — modo degradado", esp_err_to_name(err));

    err = phase_hal();
    NB_ASSERT(err == ESP_OK, TAG, "PHASE_HAL falhou: %s", esp_err_to_name(err));

    err = phase_safety();
    NB_ASSERT(err == ESP_OK, TAG, "PHASE_SAFETY falhou: %s", esp_err_to_name(err));

    err = phase_services();
    NB_ASSERT(err == ESP_OK, TAG, "PHASE_SERVICES falhou: %s", esp_err_to_name(err));

    err = phase_motion();
    NB_ASSERT(err == ESP_OK, TAG, "PHASE_MOTION falhou: %s", esp_err_to_name(err));

    phase_complete();

    /*
     * Em operação normal, o fluxo de controle passa para as tasks FreeRTOS
     * iniciadas pelas fases (render_task, behavior_task, etc.) e app_main
     * pode entrar em loop ou deletar a si mesma.
     *
     * Neste stub (Etapa 0.1), retornamos para app_main gerenciar o loop.
     */
    return ESP_OK;
}

const nb_boot_status_t *boot_manager_get_status(void)
{
    return s_initialized ? &s_status : NULL;
}

nb_boot_phase_t boot_manager_get_phase(void)
{
    return s_status.current_phase;
}

bool boot_manager_is_safe_mode(void)
{
    return s_status.safe_mode;
}

void boot_manager_report_success(void)
{
    boot_nvs_clear_fail_count();
    s_status.boot_count = 0;
    s_status.safe_mode  = false;
    NB_LOGI(TAG, "boot_count resetado — sistema reportou sucesso");
}
