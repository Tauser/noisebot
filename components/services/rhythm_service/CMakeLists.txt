idf_component_register(
    SRCS
        "rhythm_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        infra
        sound_analysis_service
)
