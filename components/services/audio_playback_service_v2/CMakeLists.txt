idf_component_register(
    SRCS
        "audio_playback_service_v2.c"
    INCLUDE_DIRS "."
    REQUIRES
        audio_io_service_v2
        esp_common
        freertos
)
