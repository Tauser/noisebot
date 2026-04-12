/*
 * sd_hal.c — HAL do microSD do NoiseBot (SDMMC 1-bit)
 */

#include "sd_hal.h"
#include "nb_hw_config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <sys/stat.h>

#define TAG "nb_sd"

static sdmmc_card_t *s_card    = NULL;
static bool          s_mounted = false;

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        if (mkdir(path, 0775) != 0) {
            ESP_LOGW(TAG, "mkdir(%s) falhou", path);
        }
    }
}

static void create_directory_structure(void)
{
    ensure_dir(NB_SD_MOUNT_POINT "/logs");
    ensure_dir(NB_SD_MOUNT_POINT "/assets");
    ensure_dir(NB_SD_MOUNT_POINT "/assets/audio");
    ensure_dir(NB_SD_MOUNT_POINT "/memory");
    ensure_dir(NB_SD_MOUNT_POINT "/config");
    ensure_dir(NB_SD_MOUNT_POINT "/snapshots");
    ESP_LOGI(TAG, "Estrutura de diretorios OK");
}

static esp_err_t do_mount(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = NB_SD_MAX_FILES,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = NB_SD_PIN_CLK;
    slot.cmd   = NB_SD_PIN_CMD;
    slot.d0    = NB_SD_PIN_DATA0;
    slot.width = NB_SD_BUS_WIDTH;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    return esp_vfs_fat_sdmmc_mount(NB_SD_MOUNT_POINT, &host, &slot,
                                   &mount_cfg, &s_card);
}

esp_err_t sd_hal_init(void)
{
    esp_err_t err = do_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD nao disponivel (%s) — modo degradado",
                 esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD montado em %s (%.1f MB)", NB_SD_MOUNT_POINT,
             (float)s_card->csd.capacity * s_card->csd.sector_size / (1024.0f * 1024.0f));
    create_directory_structure();
    return ESP_OK;
}

void sd_hal_deinit(void)
{
    if (s_mounted && s_card != NULL) {
        esp_vfs_fat_sdcard_unmount(NB_SD_MOUNT_POINT, s_card);
        s_card    = NULL;
        s_mounted = false;
        ESP_LOGI(TAG, "SD desmontado");
    }
}

bool sd_hal_is_mounted(void)
{
    return s_mounted;
}

esp_err_t sd_hal_try_remount(void)
{
    if (s_mounted) return ESP_OK;

    ESP_LOGI(TAG, "Tentando re-mount do SD...");
    esp_err_t err = do_mount();
    if (err == ESP_OK) {
        s_mounted = true;
        ESP_LOGI(TAG, "SD re-montado com sucesso");
        create_directory_structure();
    }
    return err;
}

esp_err_t sd_hal_get_free_bytes(uint64_t *out_free_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FATFS *fs;
    DWORD  free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) return ESP_FAIL;

    if (out_free_bytes) {
        *out_free_bytes = (uint64_t)free_clusters * fs->csize * 512ULL;
    }
    return ESP_OK;
}
