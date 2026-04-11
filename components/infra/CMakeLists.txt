idf_component_register(
    SRCS
        "logger.c"
        "watchdog_service.c"
        "boot_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        nvs_flash
        esp_system
        esp_task_wdt
        freertos
)
