/*
 * camera_hal_waveshare_stub.c — stub da camera no perfil Waveshare
 */

#include "camera_hal.h"

#include "esp_log.h"

static const char *TAG = "nb_cam_stub";
static bool s_logged = false;
static nb_camera_mode_t s_mode = NB_CAMERA_MODE_SAFE_QQVGA;

static void log_once(void)
{
    if (!s_logged) {
        ESP_LOGW(TAG, "camera desabilitada no perfil Waveshare");
        s_logged = true;
    }
}

esp_err_t camera_hal_set_mode(nb_camera_mode_t mode)
{
    if (mode != NB_CAMERA_MODE_SAFE_QQVGA && mode != NB_CAMERA_MODE_BETTER_QVGA) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mode = mode;
    log_once();
    return ESP_OK;
}

nb_camera_mode_t camera_hal_get_mode(void)
{
    return s_mode;
}

const char *camera_hal_mode_name(nb_camera_mode_t mode)
{
    switch (mode) {
    case NB_CAMERA_MODE_SAFE_QQVGA: return "safe";
    case NB_CAMERA_MODE_BETTER_QVGA: return "better";
    default: return "unknown";
    }
}

size_t camera_hal_mode_width(nb_camera_mode_t mode)
{
    (void)mode;
    return 0;
}

size_t camera_hal_mode_height(nb_camera_mode_t mode)
{
    (void)mode;
    return 0;
}

size_t camera_hal_effective_width(void)
{
    return 0;
}

size_t camera_hal_effective_height(void)
{
    return 0;
}

int camera_hal_last_sfmt_errno(void)
{
    return 0;
}

size_t camera_hal_mode_min_dma_before(nb_camera_mode_t mode)
{
    (void)mode;
    return 0;
}

size_t camera_hal_mode_min_dma_largest(nb_camera_mode_t mode)
{
    (void)mode;
    return 0;
}

size_t camera_hal_mode_min_internal_before(nb_camera_mode_t mode)
{
    (void)mode;
    return 0;
}

bool camera_hal_is_supported(void)
{
    return false;
}

esp_err_t camera_hal_init(void)
{
    log_once();
    return ESP_ERR_NOT_SUPPORTED;
}

bool camera_hal_is_ready(void)
{
    return false;
}

esp_err_t camera_hal_capture(void)
{
    log_once();
    return ESP_ERR_NOT_SUPPORTED;
}

nb_camera_frame_t *camera_hal_get_frame(void)
{
    return NULL;
}

void camera_hal_release_frame(void)
{
}

void camera_hal_deinit(void)
{
}
