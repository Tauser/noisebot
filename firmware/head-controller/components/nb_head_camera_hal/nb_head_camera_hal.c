/*
 * nb_head_camera_hal.c — HAL da câmera DVP OV2640 do head (Layer 1)
 *
 * Porte de camera_hal.c (main-controller). A câmera DVP compartilha DMA/SRAM
 * interna com o enlace SPI e o display; por isso abre a câmera sob demanda
 * e mantém uma sessão curta em vez de capturar continuamente.
 *
 * As funções de diagnóstico puramente informativo do original
 * (VIDIOC_ENUM_FRAMESIZES, VIDIOC_G_SENSOR_FMT, sweep de VIDIOC_TRY_FMT) não
 * foram portadas — não afetam o caminho de inicialização, só logam contexto
 * extra que ajudou a depurar o driver na primeira integração no main.
 */

#include "nb_head_camera_hal.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nb_head_i2c_hal.h"
#include "nb_hw_config_head.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef CONFIG_NB_HEAD_CAMERA_HW_ENABLED
#define CONFIG_NB_HEAD_CAMERA_HW_ENABLED 0
#endif

static const char *TAG = "nb_head_camera_hal";

#define NB_HEAD_CAM_MIN_PSRAM_FREE_AFTER_INIT (300U * 1024U)
#define NB_HEAD_CAM_WARN_DMA_FREE_AFTER_INIT (32U * 1024U)
#define NB_HEAD_CAM_WARN_INTERNAL_AFTER_INIT (48U * 1024U)
#define NB_HEAD_CAM_XCLK_FREQ_HZ 20000000
#define NB_HEAD_CAMERA_SET_FORMAT_ATTEMPTS 3U
#define NB_HEAD_CAMERA_CAPTURE_ATTEMPTS 12U
#define NB_HEAD_CAMERA_WARMUP_FRAMES 2U
#define NB_HEAD_CAMERA_PIPELINE_SETTLE_MS 250U

#if CONFIG_NB_HEAD_CAMERA_HW_ENABLED

#include "esp_psram.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#define NB_HEAD_CAMERA_VIDEO_BUFFER_COUNT 1U

static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
static int s_video_fd = -1;
static bool s_streaming = false;
static bool s_frame_borrowed = false;
static bool s_first_capture_done = false;
static uint32_t s_v4l2_format = 0;
static size_t s_frame_width = 0;
static size_t s_frame_height = 0;
static struct {
    void *start;
    size_t length;
} s_mmap_buffers[NB_HEAD_CAMERA_VIDEO_BUFFER_COUNT];
static struct v4l2_buffer s_active_buf;
static nb_head_camera_frame_t s_frame = {0};

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

    for (size_t i = 0; i < NB_HEAD_CAMERA_VIDEO_BUFFER_COUNT; ++i) {
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
    s_frame = (nb_head_camera_frame_t){0};
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

esp_err_t nb_head_camera_hal_init(void)
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
    if (psram_before < NB_HEAD_CAM_MIN_PSRAM_FREE_AFTER_INIT) {
        ESP_LOGE(TAG, "PSRAM livre antes da camera abaixo do minimo: %uKB",
                 (unsigned)(psram_before / 1024U));
        return ESP_ERR_NO_MEM;
    }

    if (!nb_head_i2c_hal_is_ready()) {
        esp_err_t i2c_err = nb_head_i2c_hal_init();
        if (i2c_err != ESP_OK && i2c_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "falha ao iniciar I2C/SCCB da camera: %s",
                     esp_err_to_name(i2c_err));
            return i2c_err;
        }
    }

    i2c_master_bus_handle_t i2c_bus = nb_head_i2c_hal_get_bus();
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
            [0] = NB_HEAD_PIN_CAM_D0,
            [1] = NB_HEAD_PIN_CAM_D1,
            [2] = NB_HEAD_PIN_CAM_D2,
            [3] = NB_HEAD_PIN_CAM_D3,
            [4] = NB_HEAD_PIN_CAM_D4,
            [5] = NB_HEAD_PIN_CAM_D5,
            [6] = NB_HEAD_PIN_CAM_D6,
            [7] = NB_HEAD_PIN_CAM_D7,
        },
        .vsync_io = NB_HEAD_PIN_CAM_VSYNC,
        .de_io = NB_HEAD_PIN_CAM_HREF,
        .pclk_io = NB_HEAD_PIN_CAM_PCLK,
        .xclk_io = NB_HEAD_PIN_CAM_XCLK,
    };

    const esp_video_init_sccb_config_t sccb_config = {
        .init_sccb = false,
        .i2c_handle = i2c_bus,
        .freq = 100000,
    };

    const esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = sccb_config,
        .reset_pin = NB_HEAD_PIN_CAM_RESET,
        .pwdn_pin = NB_HEAD_PIN_CAM_PWDN,
        .dvp_pin = dvp_pin_config,
        .xclk_freq = NB_HEAD_CAM_XCLK_FREQ_HZ,
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
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP falhou errno=%d (%s)", errno,
                 strerror(errno));
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

    struct v4l2_format current_fmt = {0};
    current_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool g_fmt_ok = (ioctl(s_video_fd, VIDIOC_G_FMT, &current_fmt) == 0);
    if (!g_fmt_ok) {
        ESP_LOGW(TAG, "VIDIOC_G_FMT falhou errno=%d (%s)", errno,
                 strerror(errno));
        current_fmt.fmt.pix.width = 0;
        current_fmt.fmt.pix.height = 0;
    }

    /* Construir o pedido de VIDIOC_S_FMT a partir da struct que o próprio
     * VIDIOC_G_FMT reportou, em vez de uma struct zerada — ver comentário
     * equivalente em camera_hal.c (main) sobre por que isso importa para o
     * driver aceitar o pedido. */
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
    bool format_set = false;
    for (uint32_t attempt = 0; attempt < NB_HEAD_CAMERA_SET_FORMAT_ATTEMPTS;
         ++attempt) {
        if (ioctl(s_video_fd, VIDIOC_S_FMT, &fmt) == 0) {
            format_set = true;
            break;
        }
        ESP_LOGW(TAG, "VIDIOC_S_FMT falhou tentativa=%lu errno=%d (%s)",
                 (unsigned long)(attempt + 1U), errno, strerror(errno));
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    if (!format_set && pixfmt != V4L2_PIX_FMT_JPEG) {
        ESP_LOGW(TAG, "VIDIOC_S_FMT YUYV rejeitado, tentando JPEG como fallback");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
        for (uint32_t attempt = 0;
             attempt < NB_HEAD_CAMERA_SET_FORMAT_ATTEMPTS; ++attempt) {
            if (ioctl(s_video_fd, VIDIOC_S_FMT, &fmt) == 0) {
                format_set = true;
                break;
            }
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
    s_v4l2_format = fmt.fmt.pix.pixelformat;
    s_frame_width = (size_t)fmt.fmt.pix.width;
    s_frame_height = (size_t)fmt.fmt.pix.height;

    struct v4l2_requestbuffers req = {0};
    req.count = NB_HEAD_CAMERA_VIDEO_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_video_fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0U) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS falhou errno=%d (%s)", errno,
                 strerror(errno));
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }

    for (uint32_t i = 0; i < req.count && i < NB_HEAD_CAMERA_VIDEO_BUFFER_COUNT;
         ++i) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(s_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF falhou errno=%d (%s)", errno,
                     strerror(errno));
            camera_hal_cleanup_video();
            vSemaphoreDelete(s_mutex);
            s_mutex = NULL;
            return ESP_FAIL;
        }
        void *start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, s_video_fd, buf.m.offset);
        if (start == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap camera falhou errno=%d (%s)", errno,
                     strerror(errno));
            camera_hal_cleanup_video();
            vSemaphoreDelete(s_mutex);
            s_mutex = NULL;
            return ESP_FAIL;
        }
        s_mmap_buffers[i].start = start;
        s_mmap_buffers[i].length = buf.length;
        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF init falhou errno=%d (%s)", errno,
                     strerror(errno));
            camera_hal_cleanup_video();
            vSemaphoreDelete(s_mutex);
            s_mutex = NULL;
            return ESP_FAIL;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON falhou errno=%d (%s)", errno,
                 strerror(errno));
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }
    s_streaming = true;
    vTaskDelay(pdMS_TO_TICKS(NB_HEAD_CAMERA_PIPELINE_SETTLE_MS));

    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t dma_after = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (psram_after < NB_HEAD_CAM_MIN_PSRAM_FREE_AFTER_INIT) {
        ESP_LOGE(TAG, "camera sem PSRAM: PSRAM=%uKB DMA=%uKB INT=%uKB",
                 (unsigned)(psram_after / 1024U),
                 (unsigned)(dma_after / 1024U),
                 (unsigned)(internal_after / 1024U));
        camera_hal_cleanup_video();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (dma_after < NB_HEAD_CAM_WARN_DMA_FREE_AFTER_INIT ||
        internal_after < NB_HEAD_CAM_WARN_INTERNAL_AFTER_INIT) {
        ESP_LOGW(TAG, "camera com headroom baixo: DMA=%uKB INT=%uKB",
                 (unsigned)(dma_after / 1024U),
                 (unsigned)(internal_after / 1024U));
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "camera pronta esp_video %lux%lu fmt=0x%08lx PSRAM=%uKB->%uKB "
             "DMA=%uKB->%uKB INT=%uKB->%uKB",
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

bool nb_head_camera_hal_is_ready(void)
{
    return s_initialized;
}

esp_err_t nb_head_camera_hal_capture(void)
{
    if (!s_initialized || !s_mutex || s_video_fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_frame_borrowed) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t warmup_target =
        s_first_capture_done ? 0U : NB_HEAD_CAMERA_WARMUP_FRAMES;
    struct v4l2_buffer buf = {0};
    bool got_frame = false;
    for (uint32_t attempt = 0; attempt < NB_HEAD_CAMERA_CAPTURE_ATTEMPTS;
         ++attempt) {
        buf = (struct v4l2_buffer){0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_DQBUF falhou tentativa=%lu errno=%d (%s)",
                     (unsigned long)(attempt + 1U), errno, strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (buf.index >= NB_HEAD_CAMERA_VIDEO_BUFFER_COUNT ||
            !s_mmap_buffers[buf.index].start ||
            buf.bytesused == 0U ||
            (s_v4l2_format != V4L2_PIX_FMT_YUV422P &&
             s_v4l2_format != V4L2_PIX_FMT_YUYV &&
             s_v4l2_format != V4L2_PIX_FMT_JPEG)) {
            ESP_LOGW(TAG, "frame invalido tentativa=%lu index=%lu bytes=%lu",
                     (unsigned long)(attempt + 1U),
                     (unsigned long)buf.index,
                     (unsigned long)buf.bytesused);
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
    ESP_LOGD(TAG, "frame capturado: %ux%u len=%u", (unsigned)s_frame.width,
             (unsigned)s_frame.height, (unsigned)s_frame.len);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

nb_head_camera_frame_t *nb_head_camera_hal_get_frame(void)
{
    return s_frame_borrowed ? &s_frame : NULL;
}

void nb_head_camera_hal_release_frame(void)
{
    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_frame_borrowed) {
        (void)ioctl(s_video_fd, VIDIOC_QBUF, &s_active_buf);
        s_frame_borrowed = false;
        s_active_buf = (struct v4l2_buffer){0};
        s_frame = (nb_head_camera_frame_t){0};
    }
    xSemaphoreGive(s_mutex);
}

void nb_head_camera_hal_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    nb_head_camera_hal_release_frame();
    camera_hal_cleanup_video();
    s_initialized = false;

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    ESP_LOGI(TAG, "camera desinicializada");
}

size_t nb_head_camera_hal_effective_width(void)
{
    return s_frame_width;
}

size_t nb_head_camera_hal_effective_height(void)
{
    return s_frame_height;
}

#else /* !CONFIG_NB_HEAD_CAMERA_HW_ENABLED */

esp_err_t nb_head_camera_hal_init(void)
{
    ESP_LOGW(TAG, "camera fisica desabilitada neste build");
    return ESP_ERR_NOT_SUPPORTED;
}

bool nb_head_camera_hal_is_ready(void)
{
    return false;
}

esp_err_t nb_head_camera_hal_capture(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

nb_head_camera_frame_t *nb_head_camera_hal_get_frame(void)
{
    return NULL;
}

void nb_head_camera_hal_release_frame(void)
{
}

void nb_head_camera_hal_deinit(void)
{
}

size_t nb_head_camera_hal_effective_width(void)
{
    return 0;
}

size_t nb_head_camera_hal_effective_height(void)
{
    return 0;
}

#endif /* CONFIG_NB_HEAD_CAMERA_HW_ENABLED */
