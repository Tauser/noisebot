#include "boot_manager.h"
#include "logger.h"

void app_main(void) {
    // BOOT_PHASE_0: watchdog, logger UART, NVS, RTC state, brownout
    // Nenhum periferico externo. Se falhar, sistema nao pode continuar.
    ESP_ERROR_CHECK(boot_manager_init());

    // BOOT_PHASE_1..N: orquestrado internamente pelo boot_manager
    // Inicia tasks FreeRTOS e retorna — scheduler assume o controle
    ESP_ERROR_CHECK(boot_manager_run());

    // app_main() pode retornar normalmente no ESP-IDF
}
