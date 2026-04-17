idf_component_register(
    SRCS
        "audio_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        nb_hal
        freertos
        log
        esp_common
        esp_timer
        sound_analysis_service
        synth_service
)
