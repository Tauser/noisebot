idf_component_register(
    SRCS
        "render_service.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        log
        esp_timer
        esp_psram
        freertos
        nb_hal
        LovyanGFX
)
