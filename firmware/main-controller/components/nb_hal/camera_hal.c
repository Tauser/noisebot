/*
 * camera_hal.c - HAL da camera DVP OV2640 (Layer 1).
 *
 * Backend esp_video/V4L2. A camera DVP compartilha
 * recursos criticos do ESP32-S3 (DMA/SRAM interna/LCD_CAM) com SD, WiFi e
 * audio; por isso o servico abre a camera sob demanda e mantem uma sessao
 * curta em vez de capturar continuamente.
 */

#include "camera_hal.h"
#include "nb_hw_config.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef NB_CAMERA_DIAG_ENABLED
#define NB_CAMERA_DIAG_ENABLED 0
#endif

#define TAG "nb_camera_hal"

#define NB_CAM_MIN_PSRAM_FREE_AFTER_INIT (300U * 1024U)
#define NB_CAM_SAFE_MIN_DMA_FREE_BEFORE_INIT    0U
#define NB_CAM_SAFE_MIN_DMA_LARGEST_BEFORE      0U
#define NB_CAM_SAFE_MIN_INTERNAL_BEFORE_INIT    0U
#define NB_CAM_BETTER_MIN_DMA_FREE_BEFORE_INIT  0U
#define NB_CAM_BETTER_MIN_DMA_LARGEST_BEFORE    0U
#define NB_CAM_BETTER_MIN_INTERNAL_BEFORE_INIT  0U
#define NB_CAM_WARN_DMA_FREE_AFTER_INIT         (32U * 1024U)
#define NB_CAM_WARN_INTERNAL_AFTER_INIT         (48U * 1024U)
#define NB_CAM_XCLK_FREQ_HZ              20000000
#define NB_CAMERA_SET_FORMAT_ATTEMPTS    3U
#define NB_CAMERA_CAPTURE_ATTEMPTS       12U
#define NB_CAMERA_WARMUP_FRAMES          2U
#define NB_CAMERA_PIPELINE_SETTLE_MS     250U

static nb_camera_mode_t s_mode = NB_CAMERA_MODE_SAFE_QQVGA;

const char *camera_hal_mode_name(nb_camera_mode_t mode)
{
    switch (mode) {
    case NB_CAMERA_MODE_SAFE_QQVGA: return "safe";
    case NB_CAMERA_MODE_BETTER_QVGA: return "better";
    default: return "unknown";
    }
}

/* Resolution is fixed by the active CONFIG_CAMERA_OV2640_DVP_* Kconfig choice
 * (see "Why resolution is fixed at build time" in docs/CAMERA_INTEGRATION.md) —
 * `mode` (safe/better) never changes it, only the memory-threshold profile
 * checked before HAL init. These constants are a non-zero fallback for
 * /api/camera/status before the first frame lands (camera_hal_effective_*()
 * is the live, driver-read source of truth and wins once a frame exists) —
 * keep them derived from Kconfig so experiment builds do not lie about the
 * native size before the first capture. */
#if CONFIG_CAMERA_OV2640_DVP_RGB565_640X480_6FPS || \
    CONFIG_CAMERA_OV2640_DVP_YUV422_640X480_6FPS || \
    CONFIG_CAMERA_OV2640_DVP_JPEG_640X480_25FPS
#define NB_CAMERA_NATIVE_FALLBACK_WIDTH  640U
#define NB_CAMERA_NATIVE_FALLBACK_HEIGHT 480U
#elif CONFIG_CAMERA_OV2640_DVP_JPEG_320X240_50FPS
#define NB_CAMERA_NATIVE_FALLBACK_WIDTH  320U
#define NB_CAMERA_NATIVE_FALLBACK_HEIGHT 240U
#elif CONFIG_CAMERA_OV2640_DVP_JPEG_1280X720_12FPS
#define NB_CAMERA_NATIVE_FALLBACK_WIDTH  1280U
#define NB_CAMERA_NATIVE_FALLBACK_HEIGHT 720U
#elif CONFIG_CAMERA_OV2640_DVP_JPEG_1600X1200_12FPS
#define NB_CAMERA_NATIVE_FALLBACK_WIDTH  1600U
#define NB_CAMERA_NATIVE_FALLBACK_HEIGHT 1200U
#elif CONFIG_CAMERA_OV2640_DVP_RAW8_800X640_30FPS || \
      CONFIG_CAMERA_OV2640_DVP_RAW8_800X640_15FPS
#define NB_CAMERA_NATIVE_FALLBACK_WIDTH  800U
#define NB_CAMERA_NATIVE_FALLBACK_HEIGHT 640U
#elif CONFIG_CAMERA_OV2640_DVP_RAW8_800X800_15FPS
#define NB_CAMERA_NATIVE_FALLBACK_WIDTH  800U
#define NB_CAMERA_NATIVE_FALLBACK_HEIGHT 800U
#elif CONFIG_CAMERA_OV2640_DVP_RAW8_1024X600_15FPS
#define NB_CAMERA_NATIVE_FALLBACK_WIDTH  1024U
#define NB_CAMERA_NATIVE_FALLBACK_HEIGHT 600U
#else
#define NB_CAMERA_NATIVE_FALLBACK_WIDTH  240U
#define NB_CAMERA_NATIVE_FALLBACK_HEIGHT 240U
#endif

size_t camera_hal_mode_width(nb_camera_mode_t mode)
{
    (void)mode;
    return NB_CAMERA_NATIVE_FALLBACK_WIDTH;
}

size_t camera_hal_mode_height(nb_camera_mode_t mode)
{
    (void)mode;
    return NB_CAMERA_NATIVE_FALLBACK_HEIGHT;
}

size_t camera_hal_mode_min_dma_before(nb_camera_mode_t mode)
{
    return (mode == NB_CAMERA_MODE_BETTER_QVGA) ?
        NB_CAM_BETTER_MIN_DMA_FREE_BEFORE_INIT :
        NB_CAM_SAFE_MIN_DMA_FREE_BEFORE_INIT;
}

size_t camera_hal_mode_min_dma_largest(nb_camera_mode_t mode)
{
    return (mode == NB_CAMERA_MODE_BETTER_QVGA) ?
        NB_CAM_BETTER_MIN_DMA_LARGEST_BEFORE :
        NB_CAM_SAFE_MIN_DMA_LARGEST_BEFORE;
}

size_t camera_hal_mode_min_internal_before(nb_camera_mode_t mode)
{
    return (mode == NB_CAMERA_MODE_BETTER_QVGA) ?
        NB_CAM_BETTER_MIN_INTERNAL_BEFORE_INIT :
        NB_CAM_SAFE_MIN_INTERNAL_BEFORE_INIT;
}

#if NB_CAMERA_DIAG_ENABLED

#include "esp_cam_sensor_types.h"
#include "esp_psram.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "i2c_hal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#define NB_CAMERA_VIDEO_BUFFER_COUNT 1U

static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
static int s_video_fd = -1;
static bool s_streaming = false;
static bool s_frame_borrowed = false;
static bool s_first_capture_done = false;
static uint32_t s_v4l2_format = 0;
static size_t s_frame_width = 0;
static size_t s_frame_height = 0;
static int s_last_sfmt_errno = 0;
static struct {
    void *start;
    size_t length;
} s_mmap_buffers[NB_CAMERA_VIDEO_BUFFER_COUNT];
static struct v4l2_buffer s_active_buf;
static nb_camera_frame_t s_frame = {0};

__attribute__((weak)) esp_err_t esp_video_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static void camera_hal_cleanup_video(void)
{
    if (s_streaming && s_video_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)ioctl(s_video_fd, VIDIOC_STREAMOFF, &type);
    }
    s_streaming = false;

    for (size_t i = 0; i < NB_CAMERA_VIDEO_BUFFER_COUNT; ++i) {
        if (s_mmap_buffers[i].start && s_mmap_buffers[i].start != MAP_FAILED) {
            (void)munmap(s_mmap_buffers[i].start, s_mmap_buffers[i].length);
        }
        s_mmap_buffers[i].start = NULL;
        s_mmap_buffers[i].length = 0;
    }

    if (s_video_fd >= 0) {
        (void)close(s_video_fd);
        s_video_fd = -1;
    }

    s_frame_borrowed = false;
    s_first_capture_done = false;
    s_v4l2_format = 0;
    s_frame_width = 0;
    s_frame_height = 0;
    s_frame = (nb_camera_frame_t){0};
    (void)esp_video_deinit();
}

static uint32_t camera_hal_choose_format(int fd)
{
    struct v4l2_fmtdesc fmtdesc = {0};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint32_t fallback_fmt = 0;

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        ESP_LOGI(TAG, "formato V4L2 disponivel: 0x%08lx %s",
                 (unsigned long)fmtdesc.pixelformat,
                 (const char *)fmtdesc.description);
        /* Prefer YUYV interleaved — allows motion analysis + software JPEG encode */
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUYV ||
            fmtdesc.pixelformat == V4L2_PIX_FMT_YUV422P) {
            return fmtdesc.pixelformat;
        }
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_JPEG) {
            fallback_fmt = V4L2_PIX_FMT_JPEG;
        }
        fmtdesc.index++;
    }

    return fallback_fmt;
}

/* Best-effort diagnostics only — VIDIOC_ENUM_FRAMESIZES is not guaranteed by
 * every V4L2 backend (esp_video may return ENOTTY/EINVAL for it). Logging its
 * result helps explain why the driver is fixed at 240x240; runtime behavior
 * must never depend on this ioctl succeeding. */
static void camera_hal_log_framesizes(int fd, uint32_t pixfmt)
{
    struct v4l2_frmsizeenum frmsize = {0};
    frmsize.pixel_format = pixfmt;

    if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) != 0) {
        ESP_LOGI(TAG, "VIDIOC_ENUM_FRAMESIZES indisponivel para fmt=0x%08lx errno=%d (%s)",
                 (unsigned long)pixfmt, errno, strerror(errno));
        return;
    }

    do {
        switch (frmsize.type) {
        case V4L2_FRMSIZE_TYPE_DISCRETE:
            ESP_LOGI(TAG, "framesize[%lu] discreto %lux%lu",
                     (unsigned long)frmsize.index,
                     (unsigned long)frmsize.discrete.width,
                     (unsigned long)frmsize.discrete.height);
            break;
        case V4L2_FRMSIZE_TYPE_STEPWISE:
        case V4L2_FRMSIZE_TYPE_CONTINUOUS:
            ESP_LOGI(TAG, "framesize[%lu] stepwise %lux%lu..%lux%lu passo %lux%lu",
                     (unsigned long)frmsize.index,
                     (unsigned long)frmsize.stepwise.min_width,
                     (unsigned long)frmsize.stepwise.min_height,
                     (unsigned long)frmsize.stepwise.max_width,
                     (unsigned long)frmsize.stepwise.max_height,
                     (unsigned long)frmsize.stepwise.step_width,
                     (unsigned long)frmsize.stepwise.step_height);
            break;
        default:
            ESP_LOGI(TAG, "framesize[%lu] tipo desconhecido=%lu",
                     (unsigned long)frmsize.index, (unsigned long)frmsize.type);
            break;
        }
        frmsize.index++;
    } while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0);
}

/* Reads back the *sensor-level* format via the esp_video private ioctl
 * (VIDIOC_G_SENSOR_FMT -> esp_cam_sensor_get_format -> dev->cur_format).
 * This is the OV2640 driver's own descriptor — including the human-readable
 * mode name baked in at build time (e.g. "DVP_8bit_20Minput_YUV422_240x240_25fps")
 * — and proves exactly which of the sensor's many compiled-in register-list
 * modes (ov2640_format_info[], up to 1600x1200 JPEG) is the one actually
 * active, as opposed to merely what the V4L2 layer reports. */
static void camera_hal_log_sensor_format(int fd)
{
    esp_cam_sensor_format_t sensor_fmt = {0};

    if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &sensor_fmt) != 0) {
        ESP_LOGI(TAG, "VIDIOC_G_SENSOR_FMT indisponivel errno=%d (%s)", errno, strerror(errno));
        return;
    }

    ESP_LOGI(TAG, "modo do sensor ativo: \"%s\" %ux%u fmt=%d fps=%u (escolhido em build-time por "
                  "CONFIG_CAMERA_OV2640_DVP_IF_FORMAT_INDEX_DEFAULT - esp_video nao expoe ioctl "
                  "para enumerar os outros modos compilados no driver OV2640)",
             sensor_fmt.name ? sensor_fmt.name : "(sem nome)",
             (unsigned)sensor_fmt.width,
             (unsigned)sensor_fmt.height,
             (int)sensor_fmt.format,
             (unsigned)sensor_fmt.fps);
}

/* Logs every field of a struct v4l2_format's pix sub-struct — not just
 * width/height/pixelformat. Used to prove (or disprove) the theory that a
 * zero-initialized v4l2_format with only width/height/pixelformat filled in
 * is an invalid request for the JPEG path on this driver — e.g. the driver
 * may require field/bytesperline/sizeimage/colorspace to match what it
 * itself reports via VIDIOC_G_FMT before it accepts TRY_FMT/S_FMT. */
static void camera_hal_log_v4l2_format(const char *label, const struct v4l2_format *fmt)
{
    ESP_LOGI(TAG, "%s: type=%lu pix={width=%lu height=%lu pixelformat=0x%08lx field=%lu "
                  "bytesperline=%lu sizeimage=%lu colorspace=%lu priv=%lu flags=%lu "
                  "ycbcr_enc=%lu quantization=%lu xfer_func=%lu}",
             label,
             (unsigned long)fmt->type,
             (unsigned long)fmt->fmt.pix.width,
             (unsigned long)fmt->fmt.pix.height,
             (unsigned long)fmt->fmt.pix.pixelformat,
             (unsigned long)fmt->fmt.pix.field,
             (unsigned long)fmt->fmt.pix.bytesperline,
             (unsigned long)fmt->fmt.pix.sizeimage,
             (unsigned long)fmt->fmt.pix.colorspace,
             (unsigned long)fmt->fmt.pix.priv,
             (unsigned long)fmt->fmt.pix.flags,
             (unsigned long)fmt->fmt.pix.ycbcr_enc,
             (unsigned long)fmt->fmt.pix.quantization,
             (unsigned long)fmt->fmt.pix.xfer_func);
}

/* Genuine VIDIOC_TRY_FMT negotiation probe (non-committing — never changes
 * driver state, unlike VIDIOC_S_FMT). Sweeps the resolutions this project has
 * historically wanted (QQVGA/QVGA/VGA) plus the native 240x240, and logs
 * exactly what the DVP V4L2 backend accepts or rejects (with errno). This
 * turns "the driver only accepts its native size" from an assumption baked
 * into a comment into an empirical, per-boot, per-resolution proof. */
static void camera_hal_probe_try_fmt(int fd, uint32_t pixfmt,
                                      const struct v4l2_format *base_fmt, bool base_valid)
{
    uint32_t native_width = base_fmt->fmt.pix.width;
    uint32_t native_height = base_fmt->fmt.pix.height;

    static const struct {
        uint32_t width;
        uint32_t height;
        const char *label;
    } candidates[] = {
        { 160U, 120U, "QQVGA" },
        { 320U, 240U, "QVGA" },
        { 640U, 480U, "VGA" },
    };

    for (size_t i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); ++i) {
        struct v4l2_format probe = {0};
        probe.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        probe.fmt.pix.width = candidates[i].width;
        probe.fmt.pix.height = candidates[i].height;
        probe.fmt.pix.pixelformat = pixfmt;

        errno = 0;
        if (ioctl(fd, VIDIOC_TRY_FMT, &probe) == 0) {
            ESP_LOGI(TAG, "VIDIOC_TRY_FMT %s %lux%lu: aceito -> driver propoe %lux%lu",
                     candidates[i].label,
                     (unsigned long)candidates[i].width,
                     (unsigned long)candidates[i].height,
                     (unsigned long)probe.fmt.pix.width,
                     (unsigned long)probe.fmt.pix.height);
        } else {
            ESP_LOGI(TAG, "VIDIOC_TRY_FMT %s %lux%lu: rejeitado errno=%d (%s) - driver so negocia "
                          "o tamanho nativo %lux%lu (compilado via Kconfig)",
                     candidates[i].label,
                     (unsigned long)candidates[i].width,
                     (unsigned long)candidates[i].height,
                     errno, strerror(errno),
                     (unsigned long)native_width,
                     (unsigned long)native_height);
        }
    }

    if (!base_valid) {
        ESP_LOGW(TAG, "probe nativo (struct preservada de G_FMT) pulado - VIDIOC_G_FMT falhou, "
                      "sem struct valida para reusar");
        return;
    }

    /* Probe the EXACT struct VIDIOC_G_FMT reported as active — copied field
     * by field (field, bytesperline, sizeimage, colorspace, priv, flags,
     * ycbcr_enc/hsv_enc, quantization, xfer_func), not a zero-initialized
     * guess with only width/height/pixelformat filled in. If the driver
     * rejects the *exact* format it itself just reported, that proves the
     * struct's other fields (not the resolution) are the source of EINVAL —
     * and the same zero-init mismatch could be corrupting VIDIOC_S_FMT,
     * which would explain the zero-byte capture buffers downstream. */
    struct v4l2_format native_probe = *base_fmt;
    native_probe.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    camera_hal_log_v4l2_format("VIDIOC_TRY_FMT nativo (struct preservada de G_FMT) - antes", &native_probe);
    errno = 0;
    if (ioctl(fd, VIDIOC_TRY_FMT, &native_probe) == 0) {
        ESP_LOGI(TAG, "VIDIOC_TRY_FMT nativo %lux%lu (struct preservada de G_FMT): aceito",
                 (unsigned long)native_width, (unsigned long)native_height);
        camera_hal_log_v4l2_format("VIDIOC_TRY_FMT nativo (struct preservada de G_FMT) - depois", &native_probe);
    } else {
        ESP_LOGW(TAG, "VIDIOC_TRY_FMT nativo %lux%lu (struct preservada de G_FMT): rejeitado "
                      "errno=%d (%s) - ate o proprio formato reportado pelo driver foi recusado",
                 (unsigned long)native_width, (unsigned long)native_height, errno, strerror(errno));
    }
}

bool camera_hal_is_supported(void)
{
    return true;
}

size_t camera_hal_effective_width(void)
{
    return s_frame_width;
}

size_t camera_hal_effective_height(void)
{
    return s_frame_height;
}

int camera_hal_last_sfmt_errno(void)
{
    return s_last_sfmt_errno;
}

esp_err_t camera_hal_set_mode(nb_camera_mode_t mode)
{
    if (mode != NB_CAMERA_MODE_SAFE_QQVGA && mode != NB_CAMERA_MODE_BETTER_QVGA) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Mode now only affects JPEG encode quality, not resolution.
     * No reinit required — can change while camera is streaming. */
    s_mode = mode;
    return ESP_OK;
}

nb_camera_mode_t camera_hal_get_mode(void)
{
    return s_mode;
}

esp_err_t camera_hal_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "camera ja inicializada");
        return ESP_ERR_INVALID_STATE;
    }

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM indisponivel para framebuffer da camera");
        return ESP_ERR_NO_MEM;
    }

    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t dma_before = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (psram_before < NB_CAM_MIN_PSRAM_FREE_AFTER_INIT) {
        ESP_LOGE(TAG, "PSRAM livre antes da camera abaixo do minimo: %uKB",
                 (unsigned)(psram_before / 1024U));
        return ESP_ERR_NO_MEM;
    }

    if (!nb_i2c_hal_is_ready()) {
        esp_err_t i2c_err = nb_i2c_hal_init();
        if (i2c_err != ESP_OK && i2c_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "falha ao iniciar I2C/SCCB da camera: %s", esp_err_to_name(i2c_err));
            return i2c_err;
        }
    }

    i2c_master_bus_handle_t i2c_bus = nb_i2c_hal_get_bus();
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2C/SCCB da camera indisponivel");
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "falha ao criar mutex da camera");
        return ESP_ERR_NO_MEM;
    }

    static const esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {
            [0] = NB_CAM_PIN_D0,
            [1] = NB_CAM_PIN_D1,
            [2] = NB_CAM_PIN_D2,
            [3] = NB_CAM_PIN_D3,
            [4] = NB_CAM_PIN_D4,
            [5] = NB_CAM_PIN_D5,
            [6] = NB_CAM_PIN_D6,
            [7] = NB_CAM_PIN_D7,
        },
        .vsync_io = NB_CAM_PIN_VSYNC,
        .de_io = NB_CAM_PIN_HREF,
        .pclk_io = NB_CAM_PIN_PCLK,
        .xclk_io = NB_CAM_PIN_XCLK,
    };

    const esp_video_init_sccb_config_t sccb_config = {
        .init_sccb = false,
        .i2c_handle = i2c_bus,
        .freq = 100000,
    };

    const esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = sccb_config,
        .reset_pin = NB_CAM_PIN_RESET,
        .pwdn_pin = NB_CAM_PIN_PWDN,
        .dvp_pin = dvp_pin_config,
        .xclk_freq = NB_CAM_XCLK_FREQ_HZ,
    };

    const esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };

    esp_err_t err = esp_video_init(&video_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init falhou: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    s_video_fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "open %s falhou errno=%d (%s)",
                 ESP_VIDEO_DVP_DEVICE_NAME, errno, strerror(errno));
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }

    struct v4l2_capability cap = {0};
    if (ioctl(s_video_fd, VIDIOC_QUERYCAP, &cap) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP falhou errno=%d (%s)", errno, strerror(errno));
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }

    uint32_t pixfmt = camera_hal_choose_format(s_video_fd);
    if (pixfmt != V4L2_PIX_FMT_YUYV &&
        pixfmt != V4L2_PIX_FMT_YUV422P &&
        pixfmt != V4L2_PIX_FMT_JPEG) {
        ESP_LOGE(TAG, "backend esp_video sem formato V4L2 suportado para snapshot");
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }

    camera_hal_log_framesizes(s_video_fd, pixfmt);
    camera_hal_log_sensor_format(s_video_fd);

    struct v4l2_format current_fmt = {0};
    current_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool g_fmt_ok = (ioctl(s_video_fd, VIDIOC_G_FMT, &current_fmt) == 0);
    if (!g_fmt_ok) {
        ESP_LOGW(TAG, "VIDIOC_G_FMT falhou errno=%d (%s)", errno, strerror(errno));
        current_fmt.fmt.pix.width = 0;
        current_fmt.fmt.pix.height = 0;
    } else {
        camera_hal_log_v4l2_format("VIDIOC_G_FMT (formato atual reportado pelo driver)", &current_fmt);
    }

    /* Real VIDIOC_TRY_FMT negotiation sweep — proves empirically (logged at
     * every boot) which resolutions the V4L2 layer would accept, instead of
     * relying on a comment's claim. TRY_FMT never commits, so this is safe to
     * run before VIDIOC_S_FMT below. The probe also reuses VIDIOC_G_FMT's
     * full struct (preserving field/bytesperline/sizeimage/etc.) for the
     * native-size check, instead of a zero-initialized guess — see
     * camera_hal_probe_try_fmt(). */
    camera_hal_probe_try_fmt(s_video_fd, pixfmt, &current_fmt, g_fmt_ok);

    /* Build the VIDIOC_S_FMT request from VIDIOC_G_FMT's own struct whenever
     * possible — preserving field, bytesperline, sizeimage, colorspace, priv,
     * flags, ycbcr_enc/hsv_enc, quantization and xfer_func exactly as the
     * driver itself reported them, instead of a zero-initialized struct that
     * only fills width/height/pixelformat. A zero-initialized request may be
     * an invalid descriptor for the JPEG path (e.g. field/colorspace == 0
     * mismatching what the driver expects) and could be why VIDIOC_S_FMT
     * "succeeds" yet the resulting capture buffers come back as 0 bytes. The
     * driver's hard equality check (see CAMERA_INTEGRATION.md "Why resolution
     * is fixed at 240x240") still means only the pixel format can meaningfully
     * vary here — width/height always end up at the build-time native size. */
    struct v4l2_format fmt;
    if (g_fmt_ok) {
        fmt = current_fmt;
    } else {
        memset(&fmt, 0, sizeof(fmt));
        fmt.fmt.pix.width = 240U;
        fmt.fmt.pix.height = 240U;
    }
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.pixelformat = pixfmt;
    camera_hal_log_v4l2_format("VIDIOC_S_FMT (struct enviada, base=G_FMT preservada quando disponivel) - antes", &fmt);
    bool format_set = false;
    s_last_sfmt_errno = 0;
    for (uint32_t attempt = 0; attempt < NB_CAMERA_SET_FORMAT_ATTEMPTS; ++attempt) {
        if (ioctl(s_video_fd, VIDIOC_S_FMT, &fmt) == 0) {
            format_set = true;
            s_last_sfmt_errno = 0;
            break;
        }
        s_last_sfmt_errno = errno;
        ESP_LOGW(TAG, "VIDIOC_S_FMT falhou tentativa=%lu errno=%d (%s)",
                 (unsigned long)(attempt + 1U),
                 errno,
                 strerror(errno));
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    if (!format_set && pixfmt != V4L2_PIX_FMT_JPEG) {
        /* YUYV rejected — fall back to JPEG which the driver accepted previously. */
        ESP_LOGW(TAG, "VIDIOC_S_FMT YUYV rejeitado, tentando JPEG como fallback");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
        camera_hal_log_v4l2_format("VIDIOC_S_FMT fallback JPEG (struct enviada) - antes", &fmt);
        for (uint32_t attempt = 0; attempt < NB_CAMERA_SET_FORMAT_ATTEMPTS; ++attempt) {
            if (ioctl(s_video_fd, VIDIOC_S_FMT, &fmt) == 0) {
                format_set = true;
                s_last_sfmt_errno = 0;
                break;
            }
            s_last_sfmt_errno = errno;
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }
    if (!format_set) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT falhou definitivamente para %lux%lu fmt=0x%08lx",
                 (unsigned long)fmt.fmt.pix.width,
                 (unsigned long)fmt.fmt.pix.height,
                 (unsigned long)fmt.fmt.pix.pixelformat);
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }
    camera_hal_log_v4l2_format("VIDIOC_S_FMT (struct retornada pelo driver) - depois", &fmt);
    s_v4l2_format = fmt.fmt.pix.pixelformat;
    s_frame_width = (size_t)fmt.fmt.pix.width;
    s_frame_height = (size_t)fmt.fmt.pix.height;

    struct v4l2_requestbuffers req = {0};
    req.count = NB_CAMERA_VIDEO_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_video_fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0U) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS falhou errno=%d (%s)", errno, strerror(errno));
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }

    for (uint32_t i = 0; i < req.count && i < NB_CAMERA_VIDEO_BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(s_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF falhou errno=%d (%s)", errno, strerror(errno));
            camera_hal_cleanup_video();
            vSemaphoreDelete(s_mutex);
            s_mutex = NULL;
            return ESP_FAIL;
        }
        void *start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, s_video_fd, buf.m.offset);
        if (start == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap camera falhou errno=%d (%s)", errno, strerror(errno));
            camera_hal_cleanup_video();
            vSemaphoreDelete(s_mutex);
            s_mutex = NULL;
            return ESP_FAIL;
        }
        s_mmap_buffers[i].start = start;
        s_mmap_buffers[i].length = buf.length;
        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF init falhou errno=%d (%s)", errno, strerror(errno));
            camera_hal_cleanup_video();
            vSemaphoreDelete(s_mutex);
            s_mutex = NULL;
            return ESP_FAIL;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON falhou errno=%d (%s)", errno, strerror(errno));
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }
    s_streaming = true;
    vTaskDelay(pdMS_TO_TICKS(NB_CAMERA_PIPELINE_SETTLE_MS));

    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t dma_after = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (psram_after < NB_CAM_MIN_PSRAM_FREE_AFTER_INIT) {
        ESP_LOGE(TAG, "camera sem PSRAM: PSRAM=%uKB DMA=%uKB INT=%uKB",
                 (unsigned)(psram_after / 1024U),
                 (unsigned)(dma_after / 1024U),
                 (unsigned)(internal_after / 1024U));
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (dma_after < NB_CAM_WARN_DMA_FREE_AFTER_INIT ||
        internal_after < NB_CAM_WARN_INTERNAL_AFTER_INIT) {
        ESP_LOGW(TAG, "camera com headroom baixo: DMA=%uKB INT=%uKB",
                 (unsigned)(dma_after / 1024U),
                 (unsigned)(internal_after / 1024U));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "camera pronta esp_video mode=%s %lux%lu fmt=0x%08lx PSRAM=%uKB->%uKB DMA=%uKB->%uKB INT=%uKB->%uKB",
             camera_hal_mode_name(s_mode),
             (unsigned long)fmt.fmt.pix.width,
             (unsigned long)fmt.fmt.pix.height,
             (unsigned long)s_v4l2_format,
             (unsigned)(psram_before / 1024U),
             (unsigned)(psram_after / 1024U),
             (unsigned)(dma_before / 1024U),
             (unsigned)(dma_after / 1024U),
             (unsigned)(internal_before / 1024U),
             (unsigned)(internal_after / 1024U));
    return ESP_OK;
}

bool camera_hal_is_ready(void)
{
    return s_initialized;
}

esp_err_t camera_hal_capture(void)
{
    if (!s_initialized || !s_mutex || s_video_fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_frame_borrowed) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Warmup only on the very first capture after init — pipeline settle
     * is already done via PIPELINE_SETTLE_MS delay in camera_hal_init(). */
    uint32_t warmup_target = s_first_capture_done ? 0U : NB_CAMERA_WARMUP_FRAMES;
    struct v4l2_buffer buf = {0};
    bool got_frame = false;
    for (uint32_t attempt = 0; attempt < NB_CAMERA_CAPTURE_ATTEMPTS; ++attempt) {
        buf = (struct v4l2_buffer){0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_DQBUF falhou tentativa=%lu errno=%d (%s)",
                     (unsigned long)(attempt + 1U),
                     errno,
                     strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (buf.index >= NB_CAMERA_VIDEO_BUFFER_COUNT ||
            !s_mmap_buffers[buf.index].start ||
            buf.bytesused == 0U ||
            (s_v4l2_format != V4L2_PIX_FMT_YUV422P &&
             s_v4l2_format != V4L2_PIX_FMT_YUYV &&
             s_v4l2_format != V4L2_PIX_FMT_JPEG)) {
            ESP_LOGW(TAG, "frame invalido tentativa=%lu index=%lu bytes=%lu fmt=0x%08lx",
                     (unsigned long)(attempt + 1U),
                     (unsigned long)buf.index,
                     (unsigned long)buf.bytesused,
                     (unsigned long)s_v4l2_format);
            (void)ioctl(s_video_fd, VIDIOC_QBUF, &buf);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (attempt < warmup_target) {
            (void)ioctl(s_video_fd, VIDIOC_QBUF, &buf);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        got_frame = true;
        s_first_capture_done = true;
        break;
    }

    if (!got_frame) {
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    s_active_buf = buf;
    s_frame_borrowed = true;
    s_frame.buf = (uint8_t *)s_mmap_buffers[buf.index].start;
    s_frame.len = buf.bytesused;
    s_frame.width = s_frame_width;
    s_frame.height = s_frame_height;
    s_frame.format = (int)s_v4l2_format;
    ESP_LOGD(TAG, "frame capturado: %ux%u len=%u",
             (unsigned)s_frame.width,
             (unsigned)s_frame.height,
             (unsigned)s_frame.len);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

nb_camera_frame_t *camera_hal_get_frame(void)
{
    return s_frame_borrowed ? &s_frame : NULL;
}

void camera_hal_release_frame(void)
{
    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_frame_borrowed) {
        (void)ioctl(s_video_fd, VIDIOC_QBUF, &s_active_buf);
        s_frame_borrowed = false;
        s_active_buf = (struct v4l2_buffer){0};
        s_frame = (nb_camera_frame_t){0};
    }
    xSemaphoreGive(s_mutex);
}

void camera_hal_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    camera_hal_release_frame();
    camera_hal_cleanup_video();
    s_initialized = false;

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    ESP_LOGI(TAG, "camera desinicializada");
}

#else

esp_err_t camera_hal_set_mode(nb_camera_mode_t mode)
{
    if (mode != NB_CAMERA_MODE_SAFE_QQVGA && mode != NB_CAMERA_MODE_BETTER_QVGA) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mode = mode;
    return ESP_OK;
}

nb_camera_mode_t camera_hal_get_mode(void)
{
    return s_mode;
}

bool camera_hal_is_supported(void)
{
    return false;
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

esp_err_t camera_hal_init(void)
{
    ESP_LOGW(TAG, "camera desabilitada neste build");
    return ESP_ERR_NOT_SUPPORTED;
}

bool camera_hal_is_ready(void)
{
    return false;
}

esp_err_t camera_hal_capture(void)
{
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

#endif
