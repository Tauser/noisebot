idf_component_register(
    SRCS
        "boredom_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        esp_timer
        esp_hw_support
        infra
        state_machine
        expression_service
        led_service
        synth_service
        ui_overlay_service
)
