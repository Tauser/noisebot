idf_component_register(
    SRCS
        "ui_overlay_service.cpp"
        "fonts/MontserratSemiBold26.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        esp_timer
        freertos
        nb_hal
        LovyanGFX
        render_service
)
