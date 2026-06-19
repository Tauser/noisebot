idf_component_register(
    SRCS
        "vad_semantic_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        esp_hw_support
        infra
        sound_analysis_service
)
