idf_component_register(
    SRCS
        "expression_service.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        log
        esp_timer
        esp_hw_support
        freertos
        nb_hal
        LovyanGFX
        render_service
        ui_overlay_service
        led_service
        visual_state_facade
)
