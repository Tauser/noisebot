idf_component_register(
    SRCS
        "audio_playback_service_v2.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_common
        freertos
)
