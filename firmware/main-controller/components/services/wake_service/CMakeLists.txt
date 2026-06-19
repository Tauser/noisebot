idf_component_register(
    SRCS
        "wake_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        esp_common
        esp_timer
        nvs_flash
        infra
        espressif__esp-sr
)
