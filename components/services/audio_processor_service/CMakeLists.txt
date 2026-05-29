idf_component_register(
    SRCS
        "audio_processor_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        esp_common
        esp_timer
        nvs_flash
        espressif__esp-sr
        espressif__esp_audio_codec
        nb_hal
)
