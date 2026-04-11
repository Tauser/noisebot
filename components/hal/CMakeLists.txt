# HAL — drivers de hardware
# Fontes adicionadas progressivamente conforme cada etapa do roadmap:
#   Etapa 1.1: display/st7789_drv.c
#   Etapa 1.2: led/ws2812_drv.c
#   Etapa 1.3: storage/sd_storage_drv.c
#   Etapa 3.1: servo/scservo_drv.c
#   Etapa 4.2: imu/mpu6050_drv.c
#   Etapa 4.1: touch/touch_drv.c
#   Etapa 5.x: audio/inmp441_drv.c  audio/max98357a_drv.c
#   Etapa 6.1: camera/ov2640_drv.c
#   Etapa 0.3: power/max17048_drv.c  power/bq25185_drv.c

idf_component_register(
    SRCS
        ""
    INCLUDE_DIRS
        "."
        "display"
        "led"
        "servo"
        "audio"
        "imu"
        "power"
        "storage"
        "camera"
        "touch"
    REQUIRES
        infra
        driver
        esp_timer
)
