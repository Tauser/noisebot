idf_component_register(
    SRCS
        "gaze_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        expression_service
        render_service
        esp_hw_support
        attention_service
)
