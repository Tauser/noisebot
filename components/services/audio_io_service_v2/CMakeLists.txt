idf_component_register(
    SRCS
        "audio_io_service_v2.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_common
        esp_timer
        freertos
        heap
)
