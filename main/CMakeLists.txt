idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        infra
        audio_service
        freertos
        log
)
