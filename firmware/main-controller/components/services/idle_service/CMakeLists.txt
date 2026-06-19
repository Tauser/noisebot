idf_component_register(
    SRCS
        "idle_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        esp_hw_support
        gaze_service
        expression_service
        state_machine
        attention_service
        rhythm_service
)
