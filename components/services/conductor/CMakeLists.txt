idf_component_register(
    SRCS
        "conductor.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        esp_timer
        esp_hw_support
        expression_service
        motion_service
        audio_service
        gaze_service
        state_machine
)
