idf_component_register(
    SRCS
        "emotion_model.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        expression_service
)
