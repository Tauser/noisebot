idf_component_register(
    SRCS
        "audio_codec_service_v2.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_common
        espressif__esp_audio_codec
        heap
)
