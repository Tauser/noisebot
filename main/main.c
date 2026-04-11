/*
 * NodeBot — main.c
 *
 * Ponto de entrada do firmware. Responsável apenas por disparar a sequência
 * de boot via boot_manager. Não contém lógica de negócio.
 *
 * Sequência de boot (implementada em infra/boot_manager):
 *   PHASE_EARLY      : UART, watchdog HW, NVS, reset reason
 *   PHASE_POWER      : brownout callback, power monitor
 *   PHASE_STORAGE    : microSD mount, persistence manager
 *   PHASE_HAL        : display, LEDs, touch, servo ping
 *   PHASE_SAFETY     : motion_safety init (servos NÃO ativos ainda)
 *   PHASE_SERVICES   : render, audio, behavior, gaze, conductor
 *   PHASE_MOTION     : servos ativados APÓS safety confirmado
 *   PHASE_COMPLETE   : boot_count reset, success flag, idle behavior
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* TODO(etapa-0.1): substituir por #include "infra/boot_manager.h" */

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "NodeBot firmware starting...");

    /*
     * TODO(etapa-0.1): boot_manager_run()
     *
     * boot_manager_run() inicializa todas as fases em ordem e nunca retorna
     * em operação normal. Em safe mode, retorna após inicializar apenas as
     * fases básicas (display + logging).
     */

    /* Placeholder: loop vazio até Etapa 0.1 estar implementada */
    ESP_LOGW(TAG, "boot_manager nao implementado — aguardando Etapa 0.1");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
