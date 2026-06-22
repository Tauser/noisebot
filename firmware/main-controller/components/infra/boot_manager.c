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
#include "sdkconfig.h"

#include "boot_manager.h"
#include "hal/usb_serial_jtag_ll.h"
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
#include "ui_overlay_service.h"
#include "visual_state_facade.h"
#include "led_service.h"
#include "touch_service.h"
#include "i2c_hal.h"
#include "audio_service.h"
// #include "servo_hal.h"
// #include "motion_safety.h"
// #include "motion_service.h"
#include "state_machine.h"
#include "emotion_model.h"
#include "gaze_service.h"
#include "idle_service.h"
#include "conductor.h"
#include "long_term_memory.h"
#include "diagnostics_service.h"
#include "schedule_service.h"
#include "behavior_engine.h"
#include "voice_controller.h"
#include "sound_analysis_service.h"
#include "synth_service.h"
#include "attention_service.h"
#include "rhythm_service.h"
#include "vad_semantic_service.h"
#include "touch_semantic_service.h"
#include "presence_semantic_service.h"
#include "persona_service.h"
#include "circadian_service.h"
#include "wifi_service.h"
#include "web_service.h"
#include "bridge_service.h"
#include "wake_service.h"
#include "audio_processor_service.h"
#include "time_service.h"
#include "agenda_service.h"
#include "camera_service.h"
#include "vision_service.h"
#include "vision_preview_service.h"
#include "nb_main_link_service.h"
#include "esp_ota_ops.h"
#include "nb_hw_config.h"
#include "nb_config_keys.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define TAG "nb_boot"

/* Macro de subscribe com verificação de erro — falha em init é fatal silenciosa
 * sem isso, pois nb_event_subscribe retorna ESP_ERR_NO_MEM sem assert. */
#define BOOT_SUBSCRIBE(type, handler, ctx) \
    do { \
        esp_err_t _sub_err = nb_event_subscribe((type), (handler), (ctx), NULL); \
        if (_sub_err != ESP_OK) { \
            NB_LOGE(TAG, "subscribe(%d) falhou: %s — handler '%s' inativo", \
                    (int)(type), esp_err_to_name(_sub_err), #handler); \
        } \
    } while (0)

/* ── Chaves NVS (namespace nb_sys) ──────────────────────────────────────── */

#define NVS_NS_SYS              "nb_sys"
#define NVS_KEY_BOOT_COUNT      "boot_count"
#define NVS_KEY_SAFE_MODE       "safe_mode"
#define NVS_KEY_RESET_REASON    "reset_reason"
#define NVS_KEY_BOOT_SUCCESS    "boot_ok"

#define NVS_NS_MILESTONE        "nb_ms"
#define NVS_KEY_MS_H100         "ms_h100"

/* Follow-up automático abre o microfone por uma janela curta após a fala do
 * robô (sem exigir wake word), para permitir continuidade de conversa. O
 * server só arma esse pedido (FOLLOWUP_ARM) quando a própria resposta do
 * robô termina em pergunta. Primeiro contato e nova fala fora dessa janela
 * continuam exigindo wake word explícita. */
#define NB_VOICE_AUTO_FOLLOWUP_ENABLED 1

/* ── Estado interno ──────────────────────────────────────────────────────── */

static nb_boot_status_t s_status = {
    .current_phase = NB_BOOT_PHASE_NONE,
    .safe_mode     = false,
    .sd_degraded   = false,
    .boot_count    = 0,
    .reset_reason  = 0,
};

static bool     s_initialized       = false;
static uint32_t s_silence_ms        = 0;    /* ms sem interação — acumula em IDLE/SLEEPING */
static bool     s_milestone_100h    = false; /* greet especial de 100h pendente ao primeiro IDLE */
static bool     s_bridge_reply_playing = false; /* PLAYBACK atual veio de SAY do bridge */
static uint32_t s_sleep_touch_guard_ms = 0; /* ignora toque residual ao entrar em SLEEPING */
static uint32_t s_sleep_wake_guard_ms  = 0; /* evita falso wake word logo após dormir */
static uint32_t s_timer_badge_tick_ms  = 0; /* atualiza badge visual do timer em 1 Hz */
static bool     s_status_bridge_connected = false;

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
    s_status.skipped_phases |= (uint16_t)(1u << (unsigned)phase);
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
    bool session_voice_evt = (data != 0U) || audio_service_is_listening();
    bool start_followup = false;
    nb_event_t bus_evt = {
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000LL),
        .data.u32     = data,
    };
    switch (evt) {
        case NB_AUDIO_EVT_VOICE_START:
            if (!session_voice_evt) return;
            s_silence_ms = 0;
            state_machine_on_voice_start();
            bus_evt.type = NB_EVT_VOICE_ACTIVITY_START;
            break;
        case NB_AUDIO_EVT_VOICE_END:
            if (!session_voice_evt) return;
            ui_overlay_listening_set(false);
            state_machine_on_voice_end();
            bus_evt.type = NB_EVT_VOICE_ACTIVITY_END;
            break;
        case NB_AUDIO_EVT_PLAYBACK_START:
            if (audio_service_is_listening()) {
                NB_LOGW(TAG, "PLAYBACK_START ignorado durante escuta ativa");
                audio_play_stop();
                ui_overlay_status_icon_set(NB_UI_STATUS_ICON_SPEAKER_ACTIVE, false);
                return;
            }
            ui_overlay_status_icon_set(NB_UI_STATUS_ICON_SPEAKER_ACTIVE, true);
            s_bridge_reply_playing = (data == 0U);
            state_machine_on_audio_started();
            bus_evt.type = NB_EVT_AUDIO_STARTED;
            break;
        case NB_AUDIO_EVT_PLAYBACK_END:
            ui_overlay_status_icon_set(NB_UI_STATUS_ICON_SPEAKER_ACTIVE, false);
            start_followup = s_bridge_reply_playing;
            s_bridge_reply_playing = false;
            if (start_followup) {
                ui_overlay_clear_text();
            }
            state_machine_on_audio_ended();
            if (start_followup &&
                bridge_service_is_connected() &&
                bridge_service_consume_followup_request()) {
#if NB_VOICE_AUTO_FOLLOWUP_ENABLED
                voice_controller_request_followup_listen();
#else
                NB_LOGI(TAG, "FOLLOWUP_ARM ignorado; aguardando wake word");
#endif
            }
            bus_evt.type = NB_EVT_AUDIO_ENDED;
            break;
        default: return;
    }
    nb_event_publish_async(&bus_evt);
}

static void on_camera_event(nb_camera_event_t evt)
{
    switch (evt) {
        case NB_CAMERA_EVT_SESSION_ACTIVE:
            idle_service_set_camera_active(true);
            break;
        case NB_CAMERA_EVT_SESSION_INACTIVE:
            idle_service_set_camera_active(false);
            break;
        default:
            break;
    }
}

/* ── Relay de eventos de touch → event bus + state machine + emotion ─────── */

static void silence_active_alert(const char *source)
{
    synth_stop();
    NB_LOGI(TAG, "alerta silenciado (%s)", source ? source : "unknown");
}

static bool state_is_intentional_silent(nb_robot_state_t state)
{
    return state == NB_STATE_MEDITATION || state == NB_STATE_SILENT_COMPANY;
}

static bool mic_blocked_status_should_show(nb_robot_state_t state)
{
    return config_get_silence_mode_enabled() || state_is_intentional_silent(state);
}

static void update_silent_status_icon(nb_robot_state_t new_state,
                                      nb_robot_state_t old_state)
{
    const bool now_silent = mic_blocked_status_should_show(new_state);
    const bool was_silent = mic_blocked_status_should_show(old_state);

    if (now_silent) {
        ui_overlay_listening_set(false);
        ui_overlay_status_icon_set(NB_UI_STATUS_ICON_MIC_BLOCKED, true);
    } else if (was_silent) {
        ui_overlay_status_icon_set(NB_UI_STATUS_ICON_MIC_BLOCKED, false);
    }
}

static void format_quick_status_time(char *out, size_t out_size)
{
    if (!out || out_size == 0U) return;
    out[0] = '\0';

    struct tm local_time;
    if (time_service_get_local_time(&local_time) == ESP_OK) {
        snprintf(out, out_size, "%02d:%02d",
                 local_time.tm_hour, local_time.tm_min);
    } else {
        snprintf(out, out_size, "--:--");
    }
}

static void show_quick_status_bar(void)
{
    char time_label[8];
    format_quick_status_time(time_label, sizeof(time_label));

    nb_ui_quick_status_t status = {
        .left_icon = s_status_bridge_connected
                   ? NB_UI_STATUS_ICON_BRIDGE_CONNECTED
                   : NB_UI_STATUS_ICON_BRIDGE_OFFLINE,
        .right_icon = NB_UI_STATUS_ICON_BATTERY_ABSENT,
        .left_label = s_status_bridge_connected ? "Bridge" : "Offline",
        .center_label = time_label,
        .right_label = "Sem bat.",
    };
    ui_overlay_show_quick_status(&status, 3600U);
}

/* Relay de touch: chama SM diretamente (contexto da touch task, não dispatcher)
 * e depois publica no bus para o behavior_engine reagir.
 * led_effect_touch() permanece aqui — é efeito imediato de HAL. */
static void on_touch_event(nb_touch_event_t evt)
{
    if (s_sleep_touch_guard_ms > 0u && evt != NB_TOUCH_EVT_WAKE) {
        NB_LOGI(TAG, "touch ignorado durante guarda de SLEEPING: evt=%d", (int)evt);
        return;
    }

    if (state_machine_get_state() == NB_STATE_SLEEPING) {
        s_silence_ms = 0;
        led_effect_touch();
        state_machine_on_touch_wake();
        nb_event_t e = { .type = NB_EVT_TOUCH_WAKE };
        nb_event_publish_async(&e);
        return;
    }

    /* TAP e SUSTAINED passam pelo touch_semantic_service (Etapa 10.4), que
     * decide se publica TAP simples, DOUBLE_TAP, SUSTAINED, WARM_PULSE, DEEP
     * ou CARESS. LONG_PRESS e WAKE continuam publicados diretamente. */
    switch (evt) {
        case NB_TOUCH_EVT_TAP:
            s_silence_ms = 0;
            led_effect_touch();           /* feedback LED imediato — não é comportamento */
            state_machine_on_touch_tap();
            touch_semantic_on_tap();      /* delega publicação ao serviço semântico */
            break;
        case NB_TOUCH_EVT_LONG_PRESS: {
            s_silence_ms = 0;
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

    touch_service_set_sleeping(new_state == NB_STATE_SLEEPING);
    expression_service_set_blink_enabled(new_state != NB_STATE_SLEEPING);
    expression_service_set_sleep_anim_enabled(new_state == NB_STATE_SLEEPING);
    expression_service_set_speaking_mouth_enabled(new_state == NB_STATE_RESPONDING);
    ui_overlay_sleep_bubble_set(new_state == NB_STATE_SLEEPING);
    update_silent_status_icon(new_state, old_state);
    if (new_state == NB_STATE_SLEEPING) {
        s_sleep_touch_guard_ms = 500u;
        s_sleep_wake_guard_ms  = 1500u;
        gaze_service_set_anchor(0.0f, 0.0f);
        gaze_service_set_target(0.0f, 0.0f);
    } else {
        s_sleep_wake_guard_ms = 0u;
    }

    if (old_state == NB_STATE_SLEEPING && new_state != NB_STATE_SLEEPING) {
        led_base_set(NB_LED_BASE_SLEEPING, false);
    }

    voice_controller_on_state_changed(new_state, old_state);

    /* Transições comportamentais não relacionadas diretamente ao pipeline de voz. */
    switch (new_state) {
        case NB_STATE_SLEEPING:
            led_base_set(NB_LED_BASE_SLEEPING, true);
            break;
        case NB_STATE_MEDITATION:
            led_base_set(NB_LED_BASE_MEDITATION, true);
            break;
        case NB_STATE_SILENT_COMPANY:
            led_base_set(NB_LED_BASE_SILENT_COMPANY, true);
            break;
        case NB_STATE_IDLE:
            if (old_state == NB_STATE_ATTENTIVE ||
                old_state == NB_STATE_TOUCH_REACTING) {
                expression_service_set(NB_EXPR_NEUTRAL, 600.0f);
            } else if (old_state == NB_STATE_MEDITATION) {
                led_base_set(NB_LED_BASE_MEDITATION, false);
                synth_stop();
                s_silence_ms = 0;
                /* Marco 100h pendente: dispara ao primeiro IDLE pós-boot. */
                if (s_milestone_100h) {
                    s_milestone_100h = false;
                    nb_event_t me = { .type = NB_EVT_MILESTONE_UPTIME_100H };
                    nb_event_publish_async(&me);
                }
            } else if (old_state == NB_STATE_SILENT_COMPANY) {
                led_base_set(NB_LED_BASE_SILENT_COMPANY, false);
                s_silence_ms = 0;
            } else if (s_milestone_100h &&
                       (old_state == NB_STATE_BOOT_UP || old_state == NB_STATE_SLEEPING)) {
                s_milestone_100h = false;
                nb_event_t me = { .type = NB_EVT_MILESTONE_UPTIME_100H };
                nb_event_publish_async(&me);
            }
            break;
        default: break;
    }

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

static void on_bridge_alert_command(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    const char *payload = (const char *)ev->data.ptr;
    if (!payload) return;
    if (strstr(payload, "\"event\":\"ALERT_COMMAND\"") &&
        strstr(payload, "\"action\":\"silence\"")) {
        silence_active_alert("bridge");
    } else if ((strstr(payload, "\"event\":\"STATUS_COMMAND\"") != NULL) ||
               (strstr(payload, "\"action\":\"status\"") != NULL) ||
               (strstr(payload, "\"action\":\"quick_status\"") != NULL)) {
        show_quick_status_bar();
    }
}

static void on_bridge_status_event(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    if (!ev) return;

    if (ev->type == NB_EVT_BRIDGE_CONNECTED) {
        s_status_bridge_connected = true;
        ui_overlay_status_icon_set(NB_UI_STATUS_ICON_BRIDGE_CONNECTED, false);
        ui_overlay_status_icon_set(NB_UI_STATUS_ICON_BRIDGE_OFFLINE, false);
        ui_overlay_status_icon_set(NB_UI_STATUS_ICON_WIFI_UNAVAILABLE, false);
    } else if (ev->type == NB_EVT_BRIDGE_DISCONNECTED) {
        s_status_bridge_connected = false;
        ui_overlay_status_icon_set(NB_UI_STATUS_ICON_BRIDGE_CONNECTED, false);
        ui_overlay_status_icon_set(NB_UI_STATUS_ICON_BRIDGE_OFFLINE, true);
    }
}

/* ── Callbacks de agenda ─────────────────────────────────────────────────── */

static bool play_agenda_audio_asset(const char *path)
{
    if (!path) return false;
    if (!sd_hal_is_mounted() && sd_hal_try_remount() != ESP_OK) {
        NB_LOGW(TAG, "audio agenda indisponivel: SD nao montado");
        return false;
    }

    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        NB_LOGW(TAG, "audio agenda nao encontrado: %s", path);
        return false;
    }

    esp_err_t err = audio_play_file(path);
    if (err != ESP_OK) {
        NB_LOGW(TAG, "audio agenda falhou %s: %s", path, esp_err_to_name(err));
        return false;
    }

    NB_LOGI(TAG, "audio agenda: %s", path);
    return true;
}

static void play_timer_done_chime(void)
{
    if (play_agenda_audio_asset(NB_SD_MOUNT_POINT "/assets/audio/timer_done.wav")) {
        return;
    }

    synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
    const nb_note_t notes[] = {
        { 523.25f, 90U },  /* C5 */
        { 659.25f, 90U },  /* E5 */
        { 783.99f, 140U }, /* G5 */
        { 0.0f,    35U },
        { 1046.5f, 180U }, /* C6 */
    };
    synth_melody(notes, (uint8_t)(sizeof(notes) / sizeof(notes[0])));
}

static void play_reminder_chime(void)
{
    if (play_agenda_audio_asset(NB_SD_MOUNT_POINT "/assets/audio/reminder_due.wav")) {
        return;
    }

    synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
    const nb_note_t notes[] = {
        { 659.25f, 140U }, /* E5 */
        { 783.99f, 180U }, /* G5 */
        { 987.77f, 220U }, /* B5 */
    };
    synth_melody(notes, (uint8_t)(sizeof(notes) / sizeof(notes[0])));
}

static void play_alarm_chime(void)
{
    if (play_agenda_audio_asset(NB_SD_MOUNT_POINT "/assets/audio/alarm_due.wav")) {
        return;
    }

    synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
    const nb_note_t notes[] = {
        { 783.99f, 120U }, /* G5 */
        { 0.0f,    35U },
        { 783.99f, 120U },
        { 0.0f,    35U },
        { 987.77f, 160U }, /* B5 */
        { 1174.66f, 220U },/* D6 */
    };
    synth_melody(notes, (uint8_t)(sizeof(notes) / sizeof(notes[0])));
}

static void update_timer_badge(void)
{
    nb_timer_t best = {0};
    bool found = false;

    for (uint8_t id = 0; id < NB_AGENDA_MAX_TIMERS; id++) {
        nb_timer_t t;
        if (!agenda_timer_get(id, &t) || !t.active) {
            continue;
        }
        if (!found || t.remaining_ms < best.remaining_ms) {
            best = t;
            found = true;
        }
    }

    if (found) {
        ui_overlay_timer_badge_set(true, best.remaining_ms);
    } else {
        ui_overlay_timer_badge_set(false, 0U);
    }
}

static void on_agenda_timer_changed(const nb_event_t *ev, void *ctx)
{
    (void)ev;
    (void)ctx;
    s_timer_badge_tick_ms = 0;
    update_timer_badge();
}

static void on_agenda_timer_done(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    uint8_t id = (uint8_t)ev->data.u32;
    nb_timer_t t;
    s_timer_badge_tick_ms = 0;
    update_timer_badge();
    play_timer_done_chime();
    if (agenda_timer_get(id, &t) && t.label[0] != '\0') {
        NB_LOGI("nb_boot", "timer %u venceu: %s", (unsigned)id, t.label);
        ui_overlay_show_toast(t.label, NB_UI_OVERLAY_INFO, 4000U);
    } else {
        NB_LOGI("nb_boot", "timer %u venceu", (unsigned)id);
    }
}

static void on_agenda_reminder_due(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    uint8_t id = (uint8_t)ev->data.u32;
    nb_reminder_t r;
    play_reminder_chime();
    if (agenda_reminder_get(id, &r) && r.label[0] != '\0') {
        NB_LOGI("nb_boot", "lembrete %u: %s", (unsigned)id, r.label);
        ui_overlay_show_toast(r.label, NB_UI_OVERLAY_INFO, 5000U);
    } else {
        NB_LOGI("nb_boot", "lembrete %u ativado", (unsigned)id);
    }
}

static void on_agenda_alarm_due(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    uint8_t id = (uint8_t)ev->data.u32;
    nb_alarm_t a;
    play_alarm_chime();
    if (agenda_alarm_get(id, &a) && a.label[0] != '\0') {
        NB_LOGI("nb_boot", "alarme %u: %s", (unsigned)id, a.label);
        ui_overlay_show_toast(a.label, NB_UI_OVERLAY_WARNING, 6000U);
    } else {
        NB_LOGI("nb_boot", "alarme %u disparou", (unsigned)id);
    }
}

/* VOICE_FOLLOWUP_TIMEOUT: gaze sweep lateral — "cadê você?" */
static void on_voice_followup_timeout(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    /* Olha para um lado aleatório: simula busca visual da fonte do som */
    float sign = (esp_random() & 1U) ? 1.0f : -1.0f;
    gaze_service_set_target(sign * 0.55f, 0.0f);
}

/* TOUCH_DEEP em IDLE → entra em meditação (11.4) */
static void on_touch_deep_for_meditation(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    state_machine_on_meditation_enter();
}

/* VOICE_SOFT: gaze inclina levemente para cima (postura curiosa) */
static void on_voice_soft(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    gaze_service_set_target(0.0f, -0.15f);
}

/* Aplica multiplicadores de idle_service combinando persona + circadiano. */
static void apply_idle_modifiers(nb_circadian_phase_t phase)
{
    float circ_saccade = (phase == NB_CIRCADIAN_DAWN) ? 0.6f :
                         (phase == NB_CIRCADIAN_DUSK) ? 0.8f : 1.0f;
    float persona_saccade = (persona_get_curiosity() > 0.6f) ? 1.5f : 1.0f;
    idle_service_set_saccade_multiplier(persona_saccade * circ_saccade);

    float yawn_mult = (phase == NB_CIRCADIAN_DAWN) ? 1.5f :
                      (phase == NB_CIRCADIAN_DUSK) ? 0.4f : 1.0f;
    idle_service_set_yawn_multiplier(yawn_mult);

    uint32_t base_s = config_get_idle_timeout_s();
    if (phase == NB_CIRCADIAN_DUSK) {
        float factor = (ltm_get_total_sessions() >= 20u) ? 0.5f : 0.7f;
        state_machine_set_idle_timeout_s((uint32_t)((float)base_s * factor));
    } else {
        state_machine_set_idle_timeout_s(base_s);
    }
}

static void on_circadian_phase(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    apply_idle_modifiers((nb_circadian_phase_t)ev->data.u32);
}

/* Wake word detectada → entra em ATTENTIVE com source WAKE_WORD (12.6) */
static void on_wake_word_detected(const nb_event_t *evt, void *ctx)
{
    (void)evt; (void)ctx;
    (void)voice_controller_on_wake_word_detected();
}



static void on_emotion_changed(nb_expression_t new_expr)
{
    /* Emoções são transitórias e podem ser disparadas por bridge/event bus.
     * Persistir cada mudança em NVS cria escrita de flash no caminho quente e
     * quebra tasks com stack externa quando o cache é desligado. O boot volta
     * para NEUTRAL por contrato de baseline de IDLE. */
    (void)new_expr;
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
        if (s_sleep_touch_guard_ms > 100u) {
            s_sleep_touch_guard_ms -= 100u;
        } else {
            s_sleep_touch_guard_ms = 0u;
        }

        nb_robot_state_t loop_state = state_machine_get_state();

        if (loop_state == NB_STATE_SLEEPING) {
            if (s_sleep_wake_guard_ms > 100u) {
                s_sleep_wake_guard_ms -= 100u;
            } else if (s_sleep_wake_guard_ms > 0u) {
                s_sleep_wake_guard_ms = 0u;
                wake_service_rearm();
            }
        } else {
            s_sleep_wake_guard_ms = 0u;
        }

        /* energy > 0.7: transições emocionais 30% mais rápidas (persona 11.1).
         * Em SLEEPING, SLEEPY é base de estado e não deve decair para NEUTRAL. */
        if (loop_state != NB_STATE_SLEEPING) {
            emotion_model_update(persona_get_energy() > 0.7f ? 130u : 100u);
        }
        attention_service_tick(100);
        rhythm_service_tick(100);
        vad_semantic_tick(100);
        touch_semantic_tick(100);
        presence_semantic_tick(100);
        circadian_tick(100);
        idle_service_update(100);
        agenda_service_tick(100);
        s_timer_badge_tick_ms += 100u;
        if (s_timer_badge_tick_ms >= 1000u) {
            s_timer_badge_tick_ms = 0;
            update_timer_badge();
        }
        ltm_tick(100);

        /* Timer de companhia silenciosa: 2h sem interação → SILENT_COMPANY. */
        {
            nb_robot_state_t st2 = state_machine_get_state();
            if (st2 == NB_STATE_IDLE || st2 == NB_STATE_SLEEPING) {
                s_silence_ms += 100u;
                if (s_silence_ms >= 7200000U && st2 == NB_STATE_IDLE) {
                    s_silence_ms = 0;
                    state_machine_on_silent_company_enter();
                }
            } else if (st2 != NB_STATE_SILENT_COMPANY) {
                s_silence_ms = 0;
            }
        }

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

        /* Diagnostics a cada 5s (50 x 100ms): alimenta telemetria da Home. */
        if (++stats_tick >= 50u) {
            diagnostics_collect();
            stats_tick = 0;
        }

        /* LTM flush a cada 5min (3000 × 100ms) + refresh de persona. */
        if (++ltm_tick_n >= 3000u) {
            ltm_flush();
            persona_service_refresh();
            apply_idle_modifiers(circadian_get_phase());
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

    /* GPIO 19/20 são USB D+/D- no ESP32-S3. O PHY USB fica ativo por padrão
     * após reset e gera ruído elétrico que corrompe intermitentemente o init
     * SPI do display. Desabilitar aqui, antes de qualquer HAL, garante um
     * barramento SPI limpo. bridge_service_init() (PHASE_SERVICES) reabilita
     * via usb_serial_jtag_driver_install() quando o bridge for necessário. */
    usb_serial_jtag_ll_phy_enable_pad(false);

    esp_err_t err;

    /* Display (Etapa 1.1) */
#if defined(CONFIG_NB_BOARD_PROFILE_WAVESHARE) && CONFIG_NB_BOARD_PROFILE_WAVESHARE
    NB_LOGI(TAG, "perfil Waveshare ativo — display fisico local nao inicializado");
#else
    err = display_hal_init();
    if (err != ESP_OK) {
        if (s_status.safe_mode) {
            NB_LOGW(TAG, "display_hal_init falhou em safe mode: %s — continuando sem display",
                    esp_err_to_name(err));
        } else {
            NB_ASSERT_FATAL(false, TAG, "display_hal_init falhou: %s", esp_err_to_name(err));
        }
    }
#endif

    /* LEDs (Etapa 2.1) */
    err = led_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "led_service_init falhou: %s — LEDs desativados", esp_err_to_name(err));
    } else {
        led_set_brightness(config_get_brightness());
        BaseType_t rc;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
        rc = xTaskCreateWithCaps(led_update_task, "led_task",
                                 2048, NULL, 3, NULL,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        rc = xTaskCreate(led_update_task, "led_task",
                         2048, NULL, 3, NULL);
#endif
        NB_ASSERT(rc == pdPASS, TAG, "xTaskCreate led_task falhou");
    }

    /* Touch (Etapa 2.2) */
    {
        uint8_t touch_sens_pct = config_get_touch_sensitivity();
        float sens_factor = ((float)touch_sens_pct * 0.2f) / 100.0f;

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

    /* Servo HAL — desativado temporariamente para reduzir uso de memória.
     * Reativar quando motion_service for reintegrado ao boot. */

    /* I2C (DMM.10) — bus compartilhado para sensores opcionais (IMU,
     * bateria, proximidade...). Nenhum sensor é presumido presente: o scan
     * só populariza o cache lido por GET /api/i2c, board_caps continua sem
     * assumir hardware que pode não estar instalado. */
    err = nb_i2c_hal_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "nb_i2c_hal_init falhou: %s — I2C desativado",
                esp_err_to_name(err));
    } else {
        size_t i2c_count = 0;
        nb_i2c_hal_scan(NULL, 0, &i2c_count);
        NB_LOGI(TAG, "I2C scan: %u dispositivo(s) no barramento",
                (unsigned)i2c_count);
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

    phase_skip(NB_BOOT_PHASE_SAFETY, "motion desativado temporariamente");
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

    err = nb_main_link_service_init();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        NB_LOGI(TAG, "enlace dual-MCU desabilitado por configuração");
    } else if (err != ESP_OK) {
        NB_LOGW(TAG, "nb_main_link_service_init falhou: %s — head degradado",
                esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        const esp_err_t facade_err =
            visual_state_facade_init(nb_main_link_service_queue_display);
        if (facade_err != ESP_OK) {
            NB_LOGW(TAG, "visual_state_facade_init falhou: %s",
                    esp_err_to_name(facade_err));
        }
    }

    if (s_status.safe_mode) {
        phase_skip(NB_BOOT_PHASE_SERVICES, "safe mode ativo — servicos de dominio pulados");
        return ESP_OK;
    }

#if defined(CONFIG_NB_BOARD_PROFILE_WAVESHARE) && CONFIG_NB_BOARD_PROFILE_WAVESHARE
    NB_LOGI(TAG, "perfil Waveshare ativo — renderer/UI overlay locais pulados");
#else
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

    /* vision_preview_service: desabilitado por padrão. A task de captura só
     * sobe quando habilitado via API — poupa 6KB de SRAM interna para o bridge. */
    err = vision_preview_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "vision_preview_service_init falhou: %s — preview desabilitado",
                esp_err_to_name(err));
    }

    /* expression_service (Etapa 1.3): face procedural + blink */
    err = expression_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "expression_service_init falhou: %s",
              esp_err_to_name(err));

    /* ui_overlay_service: feedback visual local para tools/bridge */
    err = ui_overlay_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "ui_overlay_service_init falhou: %s",
              esp_err_to_name(err));
    ui_overlay_status_icon_set(NB_UI_STATUS_ICON_MIC_BLOCKED,
                               config_get_silence_mode_enabled());
#endif

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
    BOOT_SUBSCRIBE(NB_EVT_VOICE_FOLLOWUP_TIMEOUT, on_voice_followup_timeout,    NULL);
    BOOT_SUBSCRIBE(NB_EVT_VOICE_SOFT,             on_voice_soft,                NULL);
    BOOT_SUBSCRIBE(NB_EVT_TOUCH_DEEP,             on_touch_deep_for_meditation, NULL);

    /* touch_semantic_service (Etapa 10.4): double-tap, DEEP, CARESS, WARM_PULSE */
    err = touch_semantic_init();
    NB_ASSERT(err == ESP_OK, TAG, "touch_semantic_init falhou: %s",
              esp_err_to_name(err));

    /* presence_semantic_service (SPEC_PRESENCA_SOCIAL_2026-06-11 F1) */
    err = presence_semantic_init();
    NB_ASSERT(err == ESP_OK, TAG, "presence_semantic_init falhou: %s",
              esp_err_to_name(err));

    /* Heurística local de presença: polling contínuo offline-first.
     * Sem isso, NB_EVT_PRESENCE_DETECTED nunca é publicado sem server.
     * 0 = usa NB_VISION_POLL_DEFAULT_INTERVAL_MS (300ms). */
    if (camera_service_is_supported()) {
        err = vision_service_poll_start(0U);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "vision_service_poll_start falhou: %s — heurística offline inativa",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "camera_service sem suporte no boot — heurística offline inativa");
    }

    /* state_machine e emotion_model (Etapa 5.1) */
    err = state_machine_init(config_get_idle_timeout_s(),
                              s_status.safe_mode,
                              on_state_changed);
    NB_ASSERT(err == ESP_OK, TAG, "state_machine_init falhou: %s",
              esp_err_to_name(err));

    err = voice_controller_init();
    NB_ASSERT(err == ESP_OK, TAG, "voice_controller_init falhou: %s",
              esp_err_to_name(err));

    /* NVS pode ter SLEEPY gravado de um boot anterior ao fix. Substituir por
     * NEUTRAL garante que o robot nunca inicie com expressão de dormindo. */
    nb_expression_t boot_emotion = (nb_expression_t)config_get_last_emotion();
    if (boot_emotion == NB_EXPR_SLEEPY) boot_emotion = NB_EXPR_NEUTRAL;
    err = emotion_model_init(boot_emotion, on_emotion_changed);
    NB_ASSERT(err == ESP_OK, TAG, "emotion_model_init falhou: %s",
              esp_err_to_name(err));

    /* behavior_task: ticks a 10Hz para idle timeout e decaimento emocional.
     * Stack 4096: emotion_model_update → expression_service_set (C++) usa frame
     * considerável — 2048 causa overflow. */
    {
        /* Stack interna: behavior_task pode chamar persona_service_refresh(),
         * que grava NVS/flash. Operações de flash desabilitam cache/PSRAM e
         * exigem stack acessível com cache desligado. */
        BaseType_t rc = xTaskCreate(behavior_task, "behav_task",
                                    4096, NULL, 5, NULL);
        NB_ASSERT(rc == pdPASS, TAG, "xTaskCreate behav_task falhou");
    }

    /* gaze_service (Etapa 5.2 / DM2.9): saccade + micro-drift.
     * Perfil legado: registra render layer z=5 no render_service local.
     * Perfil Waveshare: usa task standalone, sem render_service local. */
#if defined(CONFIG_NB_BOARD_PROFILE_WAVESHARE) && CONFIG_NB_BOARD_PROFILE_WAVESHARE
    err = gaze_service_init_standalone();
    NB_ASSERT(err == ESP_OK, TAG, "gaze_service_init_standalone falhou: %s",
              esp_err_to_name(err));
#else
    err = gaze_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "gaze_service_init falhou: %s",
              esp_err_to_name(err));
#endif

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

    /* Marco 100h: verifica uma vez por lifetime e agenda para o primeiro IDLE. */
    if (ltm_get_hours_alive() >= 100U) {
        nvs_handle_t ms_h;
        if (nvs_open(NVS_NS_MILESTONE, NVS_READWRITE, &ms_h) == ESP_OK) {
            uint8_t done = 0;
            nvs_get_u8(ms_h, NVS_KEY_MS_H100, &done);
            if (!done) {
                nvs_set_u8(ms_h, NVS_KEY_MS_H100, 1);
                nvs_commit(ms_h);
                s_milestone_100h = true;
                NB_LOGI(TAG, "milestone: 100h de uptime atingido!");
            }
            nvs_close(ms_h);
        }
    }

    /* persona_service (Etapa 11.1): dimensões de personalidade emergente */
    err = persona_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "persona_service_init falhou: %s — persona em defaults",
                esp_err_to_name(err));
    }
    /* circadian_service (Etapa 11.2): fases DAWN/DAY/DUSK por uptime de sessão */
    err = circadian_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "circadian_service_init falhou: %s",
              esp_err_to_name(err));
    BOOT_SUBSCRIBE(NB_EVT_CIRCADIAN_PHASE_CHANGED, on_circadian_phase, NULL);
    apply_idle_modifiers(circadian_get_phase());   /* aplica DAWN no boot */

    /* behavior_engine (Etapa 9.3): tabela de regras — subscreve ao event bus */
    err = behavior_engine_init();
    NB_ASSERT(err == ESP_OK, TAG, "behavior_engine_init falhou: %s",
              esp_err_to_name(err));

    /* motion_safety fault → SM: subscriber direto (não via behavior_engine,
     * pois state_machine_on_motion_fault é SM input, não comportamento). */
    BOOT_SUBSCRIBE(NB_EVT_MOTION_FAULT, on_motion_fault_event, NULL);

    /* schedule_service (Etapa 9.2): timers one-shot e recorrentes sem task */
    err = schedule_service_init();
    NB_ASSERT(err == ESP_OK, TAG, "schedule_service_init falhou: %s",
              esp_err_to_name(err));

    /* time_service (Etapa 13.1): SNTP + timezone configurável */
    err = time_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "time_service_init falhou: %s — sem hora local", esp_err_to_name(err));
    }

    /* agenda_service (Etapa 13.1): timers, lembretes e alarmes locais */
    err = agenda_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "agenda_service_init falhou: %s — agenda desativada",
                esp_err_to_name(err));
    } else {
        BOOT_SUBSCRIBE(NB_EVT_TIMER_STARTED,   on_agenda_timer_changed, NULL);
        BOOT_SUBSCRIBE(NB_EVT_TIMER_UPDATED,   on_agenda_timer_changed, NULL);
        BOOT_SUBSCRIBE(NB_EVT_TIMER_CANCELLED, on_agenda_timer_changed, NULL);
        BOOT_SUBSCRIBE(NB_EVT_TIMER_DONE,      on_agenda_timer_done,    NULL);
        BOOT_SUBSCRIBE(NB_EVT_REMINDER_DUE,    on_agenda_reminder_due,  NULL);
        BOOT_SUBSCRIBE(NB_EVT_ALARM_DUE,       on_agenda_alarm_due,     NULL);
        update_timer_badge();
    }

    /* Callback de sessão da câmera. A heurística offline de presença pode ter
     * inicializado vision/camera acima; endpoints ainda compartilham o mesmo
     * camera_service e capturam sob demanda. */
    camera_service_set_event_cb(on_camera_event);

    BOOT_SUBSCRIBE(NB_EVT_BRIDGE_SESSION,      on_bridge_alert_command, NULL);
    BOOT_SUBSCRIBE(NB_EVT_BRIDGE_CONNECTED,    on_bridge_status_event,  NULL);
    BOOT_SUBSCRIBE(NB_EVT_BRIDGE_DISCONNECTED, on_bridge_status_event,  NULL);

    /* diagnostics_service (Etapa 9.1): observabilidade e health score */
    {
        const nb_diagnostics_config_t diag_cfg = {
#if defined(CONFIG_NB_BOARD_PROFILE_WAVESHARE) && CONFIG_NB_BOARD_PROFILE_WAVESHARE
            .get_fps = NULL,
#else
            .get_fps = render_service_get_fps,
#endif
        };
        err = diagnostics_init(&diag_cfg);
        NB_ASSERT(err == ESP_OK, TAG, "diagnostics_init falhou: %s",
                  esp_err_to_name(err));
    }

    /* wifi_service (Etapa 9.6): conecta ao AP em background, mDNS noisebot.local */
    err = wifi_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "wifi_service_init falhou: %s — sem WiFi", esp_err_to_name(err));
    }

    /* web_service (Etapa 15.1): Companion API HTTP, inicia após IP adquirido */
    web_service_init();

    /* bridge_service (Etapa 12.1): bridge LLM via TCP/UART, offline-first */
    err = bridge_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "bridge_service_init falhou: %s", esp_err_to_name(err));
    }

    /* wake_service (Etapa 12.6): wake word "Hi ESP" via WakeNet9 */
    err = wake_service_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "wake_service_init falhou: %s — wake word desativado",
                esp_err_to_name(err));
    } else {
        BOOT_SUBSCRIBE(NB_EVT_WAKE_WORD_DETECTED, on_wake_word_detected, NULL);
    }

    /* audio_processor_service (Voice Pipeline Fase 5): probe AFE_TYPE_VC
     * opt-in por NVS. Não altera o pipeline principal de áudio. */
    err = audio_processor_service_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        NB_LOGW(TAG, "audio_processor_service_init falhou: %s",
                esp_err_to_name(err));
    }

    phase_ok(NB_BOOT_PHASE_SERVICES);
    return ESP_OK;
}

/*
 * PHASE_MOTION — desativada temporariamente.
 * Reativar quando servo_hal/motion_safety/motion_service forem reintegrados.
 */
static esp_err_t phase_motion(void)
{
    phase_enter(NB_BOOT_PHASE_MOTION);
    phase_skip(NB_BOOT_PHASE_MOTION, "motion desativado temporariamente");
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

    /* Resumo de fases puladas — visibilidade de capability degradada (F06). */
    if (s_status.skipped_phases != 0u) {
        NB_LOGW(TAG, "Fases puladas neste boot:");
        for (int p = 0; p <= NB_BOOT_PHASE_COMPLETE; p++) {
            if (s_status.skipped_phases & (uint16_t)(1u << (unsigned)p)) {
                NB_LOGW(TAG, "  PULADA: %s", phase_name((nb_boot_phase_t)p));
            }
        }
    }

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

    /* Transição de LED: BOOT → IDLE (azul/ciano baixo fixo). */
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
    /* Confirma firmware OTA como válido — cancela rollback automático. */
    esp_ota_mark_app_valid_cancel_rollback();
}
