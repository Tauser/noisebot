idf_component_register(
    SRCS
        "vision_preview_service.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        log
        esp_timer
        esp_psram
        freertos
        nb_hal
        LovyanGFX
        camera_service
        render_service
        infra
)
