/*
 * usb_msc.c - USB Mass Storage Class
 * Exposes the SPI flash FAT partition as a USB drive
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "config.h"
#include "usb_msc.h"
#include "fatfs_manager.h"

static const char *TAG = "USB_MSC";

// TinyUSB MSC callbacks
static int8_t msc_cb_read(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
static int8_t msc_cb_write(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
static int32_t msc_cb_sector_count(uint8_t lun);
static int32_t msc_cb_sector_size(uint8_t lun);

// MSC storage context
static tusb_msc_callback_t msc_cb = {
    .read = msc_cb_read,
    .write = msc_cb_write,
    .sector_count = msc_cb_sector_count,
    .sector_size = msc_cb_sector_size,
};

// Forward declaration of fatfs sector read/write
// We access the wear-levelled partition directly via block device API
#include "wear_levelling.h"
#include "diskio_impl.h"
#include "ffconf.h"

extern wl_handle_t wl_handle;

static int8_t msc_cb_read(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    size_t bytes_read = 0;
    esp_err_t ret = wl_read(wl_handle, lba * 512, buffer, bufsize);
    if (ret != ESP_OK) return -1;
    return 0;
}

static int8_t msc_cb_write(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    esp_err_t ret = wl_erase_range(wl_handle, lba * 512, bufsize);
    if (ret != ESP_OK) return -1;
    ret = wl_write(wl_handle, lba * 512, buffer, bufsize);
    if (ret != ESP_OK) return -1;
    return 0;
}

static int32_t msc_cb_sector_count(uint8_t lun)
{
    // Get partition size
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

    // Initialize TinyUSB stack
    tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .external_phy = false,
        .configuration_descriptor = NULL,
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    // Register MSC callbacks
    tusb_msc_set_callback(0, &msc_cb, TUSB_MSC_CB_ALL);

    ESP_LOGI(TAG, "USB MSC initialized - connect USB to access storage");
}