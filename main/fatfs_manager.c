/*
 * fatfs_manager.c - Mount FAT filesystem on SPI flash partition
 * Uses wear levelling for flash lifetime
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "config.h"
#include "fatfs_manager.h"

static const char *TAG = "FATFS";

static wl_handle_t wl_handle = WL_INVALID_HANDLE;
static bool mounted = false;

bool fatfs_mount_spiflash(void)
{
    ESP_LOGI(TAG, "Mounting FATFS on partition '%s'", STORAGE_PARTITION_LABEL);

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = STORAGE_MAX_FILES,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE * 8,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        STORAGE_MOUNT_POINT,
        STORAGE_PARTITION_LABEL,
        &mount_config,
        &wl_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(err));
        return false;
    }

    mounted = true;
    ESP_LOGI(TAG, "FATFS mounted at %s", STORAGE_MOUNT_POINT);

    // Create default directories
    mkdir(MUSIC_DIR, 0777);
    mkdir(IMAGE_DIR, 0777);

    return true;
}

bool fatfs_unmount(void)
{
    if (!mounted) return true;

    esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(STORAGE_MOUNT_POINT, wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unmount failed: %s", esp_err_to_name(err));
        return false;
    }
    mounted = false;
    wl_handle = WL_INVALID_HANDLE;
    ESP_LOGI(TAG, "FATFS unmounted");
    return true;
}

bool fatfs_file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

uint32_t fatfs_get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_size;
}

bool fatfs_read_file(const char *path, uint8_t *buf, size_t *size, size_t max_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", path);
        return false;
    }
    size_t read = fread(buf, 1, max_size, f);
    fclose(f);
    if (size) *size = read;
    return read > 0;
}