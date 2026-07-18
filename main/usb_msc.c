/*
 * usb_msc.c - USB Mass Storage exposing the SPI flash partition
 * Uses the esp_tinyusb MSC storage API
 */

#include <stdio.h>
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "wear_levelling.h"
#include "esp_partition.h"
#include "config.h"
#include "usb_msc.h"

static const char *TAG = "USB_MSC";

extern wl_handle_t wl_handle;

static int32_t msc_cb_read(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    esp_err_t ret = wl_read(wl_handle, lba * 512 + offset, buffer, bufsize);
    return (ret == ESP_OK) ? 0 : -1;
}

static int32_t msc_cb_write(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    esp_err_t ret = wl_erase_range(wl_handle, lba * 512 + offset, bufsize);
    if (ret != ESP_OK) return -1;
    ret = wl_write(wl_handle, lba * 512 + offset, buffer, bufsize);
    return (ret == ESP_OK) ? 0 : -1;
}

static int32_t msc_cb_sector_count(uint8_t lun)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, STORAGE_PARTITION_LABEL);
    if (!part) return 0;
    return part->size / 512;
}

static int32_t msc_cb_sector_size(uint8_t lun)
{
    return 512;
}

void usb_msc_init(void)
{
    ESP_LOGI(TAG, "Initializing USB MSC");

    tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .external_phy = false,
        .configuration_descriptor = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    // Register MSC callbacks using the storage API
    const tusb_msc_storage_config_t storage_cfg = {
        .lun = 0,
        .callback = {
            .read = msc_cb_read,
            .write = msc_cb_write,
            .sector_count = msc_cb_sector_count,
            .sector_size = msc_cb_sector_size,
        },
    };
    tusb_msc_storage_init(&storage_cfg);

    ESP_LOGI(TAG, "USB MSC ready - connect USB for storage access");
}