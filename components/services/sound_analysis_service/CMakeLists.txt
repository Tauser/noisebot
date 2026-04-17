idf_component_register(
    SRCS "sound_analysis_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        infra
        freertos
        log
        esp_timer
)
