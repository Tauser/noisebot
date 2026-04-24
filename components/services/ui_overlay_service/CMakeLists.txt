idf_component_register(
    SRCS
        "ui_overlay_service.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        log
        esp_timer
        freertos
        nb_hal
        LovyanGFX
        render_service
)
