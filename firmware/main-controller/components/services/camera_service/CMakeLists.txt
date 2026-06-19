idf_component_register(
    SRCS
        "camera_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        esp_hw_support
        esp_timer
        esp_new_jpeg
        esp_video
        log
        nb_hal
)
