idf_component_register(
    SRCS
        "vision_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        camera_service
        infra
        esp_timer
        freertos
        log
)
