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
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_log.h"

#include "boot_manager.h"
#include "logger.h"
#include "watchdog_service.h"
#include "error_policy.h"
#include "config_manager.h"

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

static void phase_stub(nb_boot_phase_t phase, const char *etapa)
{
    NB_LOGI(TAG, "FASE %s: stub — implementar na %s", phase_name(phase), etapa);
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

    /* Alimentar watchdog após operações lentas de NVS. */
    nb_watchdog_feed();

    phase_ok(NB_BOOT_PHASE_EARLY);
    return ESP_OK;
}

/*
 * PHASE_POWER — Etapa 0.4
 * Stub: brownout callback e power monitor.
 */
static esp_err_t phase_power(void)
{
    phase_enter(NB_BOOT_PHASE_POWER);
    phase_stub(NB_BOOT_PHASE_POWER, "Etapa 0.4");
    phase_ok(NB_BOOT_PHASE_POWER);
    return ESP_OK;
}

/*
 * PHASE_STORAGE — Etapa 0.3
 * Stub: microSD mount e persistence_manager.
 * Falha aqui é não-crítica (modo SD-degradado).
 */
static esp_err_t phase_storage(void)
{
    phase_enter(NB_BOOT_PHASE_STORAGE);
    phase_stub(NB_BOOT_PHASE_STORAGE, "Etapa 0.3");

    /*
     * Quando implementado, falha no mount deve:
     *   s_status.sd_degraded = true;
     *   NB_LOGW(TAG, "SD nao disponivel — modo degradado");
     *   return ESP_OK;  // nao fatal
     */

    phase_ok(NB_BOOT_PHASE_STORAGE);
    return ESP_OK;
}

/*
 * PHASE_HAL — Bloco 1-2
 * Stub: display, LEDs, touch, servo PING.
 */
static esp_err_t phase_hal(void)
{
    phase_enter(NB_BOOT_PHASE_HAL);
    phase_stub(NB_BOOT_PHASE_HAL, "Blocos 1-2");
    phase_ok(NB_BOOT_PHASE_HAL);
    return ESP_OK;
}

/*
 * PHASE_SAFETY — Etapa 3.2
 * Stub: motion_safety init (sem torque ainda).
 * Em safe mode: pulada.
 */
static esp_err_t phase_safety(void)
{
    phase_enter(NB_BOOT_PHASE_SAFETY);

    if (s_status.safe_mode) {
        phase_skip(NB_BOOT_PHASE_SAFETY, "safe mode ativo");
        return ESP_OK;
    }

    phase_stub(NB_BOOT_PHASE_SAFETY, "Etapa 3.2");
    phase_ok(NB_BOOT_PHASE_SAFETY);
    return ESP_OK;
}

/*
 * PHASE_SERVICES — Bloco 4-5
 * Stub: render, audio, behavior, conductor.
 * Em safe mode: pulada.
 */
static esp_err_t phase_services(void)
{
    phase_enter(NB_BOOT_PHASE_SERVICES);

    if (s_status.safe_mode) {
        phase_skip(NB_BOOT_PHASE_SERVICES, "safe mode ativo");
        return ESP_OK;
    }

    phase_stub(NB_BOOT_PHASE_SERVICES, "Blocos 4-5");
    phase_ok(NB_BOOT_PHASE_SERVICES);
    return ESP_OK;
}

/*
 * PHASE_MOTION — Etapa 3.3+
 * Stub: servos ARMED.
 * Em safe mode: pulada.
 */
static esp_err_t phase_motion(void)
{
    phase_enter(NB_BOOT_PHASE_MOTION);

    if (s_status.safe_mode) {
        phase_skip(NB_BOOT_PHASE_MOTION, "safe mode ativo");
        return ESP_OK;
    }

    phase_stub(NB_BOOT_PHASE_MOTION, "Etapa 3.3");
    phase_ok(NB_BOOT_PHASE_MOTION);
    return ESP_OK;
}

/*
 * PHASE_COMPLETE — boot concluído.
 */
static esp_err_t phase_complete(void)
{
    phase_enter(NB_BOOT_PHASE_COMPLETE);

    if (s_status.safe_mode) {
        NB_LOGW(TAG, "Boot completo em SAFE MODE — motion desabilitado");
    } else {
        NB_LOGI(TAG, "Boot completo — sistema operacional");
    }

    /* Reportar sucesso (reseta boot_count no NVS). */
    boot_manager_report_success();

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
