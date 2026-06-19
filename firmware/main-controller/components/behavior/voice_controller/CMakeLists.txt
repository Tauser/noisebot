idf_component_register(
    SRCS
        "voice_controller.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        state_machine
        audio_service
        wake_service
        synth_service
        led_service
        ui_overlay_service
)
