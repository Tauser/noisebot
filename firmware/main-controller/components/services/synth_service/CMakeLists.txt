idf_component_register(
    SRCS "synth_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        infra
        freertos
        log
        esp_hw_support
)
