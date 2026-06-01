idf_component_register(
    SRCS
        "voice_capture_session_v2.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_common
        freertos
    PRIV_REQUIRES
        infra
)
