idf_component_register(
    SRCS
        "voice_activity_service_v2.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_common
        freertos
)
