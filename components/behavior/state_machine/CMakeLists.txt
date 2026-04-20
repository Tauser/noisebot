idf_component_register(
    SRCS
        "state_machine.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        esp_timer
        audio_service
)
