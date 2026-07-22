/*
 * usb_msc.c - USB Composite Device (CDC ACM + MSC U盘)
 * CDC ACM for serial output, MSC exposes SPI Flash FATFS as USB drive
 * Default: APP mode (ESP32 reads files via VFS at /spiflash)
 * When USB connected: PC sees U disk + Serial port
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"
#include "config.h"
#include "usb_msc.h"

static const char *TAG = "USB_MSC";

tinyusb_msc_storage_handle_t storage_hdl = NULL;

/* Redirect esp_log to CDC ACM */
static int cdc_log_vprintf(const char *fmt, va_list args)
{
    char buf[512];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t*)buf, len);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
    return len;
}

void usb_msc_init(void)
{
    ESP_LOGI(TAG, "Initializing USB Composite (CDC ACM + MSC)");

    // Step 1: Mount FATFS with wear levelling on storage partition
    // This registers VFS at /spiflash so fopen() works
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 10,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE * 8,
    };
    wl_handle_t wl_handle;
    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(
        "/spiflash",
        STORAGE_PARTITION_LABEL,
        &mount_config,
        &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "FATFS mounted at /spiflash");

    // Step 2: Register storage for TinyUSB MSC (so PC sees U disk)
    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .medium.wl_handle = wl_handle,
        .fat_fs = {
            .base_path = "/spiflash",
            .config.max_files = 10,
            .format_flags = 0,
        },
    };

    ret = tinyusb_msc_new_storage_spiflash(&storage_cfg, &storage_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create storage: %s", esp_err_to_name(ret));
        return;
    }

    // Step 3: Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB: %s", esp_err_to_name(ret));
        return;
    }

    // Step 4: Init CDC ACM (virtual serial port)
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ret = tinyusb_cdcacm_init(&acm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init CDC ACM: %s", esp_err_to_name(ret));
        return;
    }

    // Step 5: Redirect esp_log to CDC ACM
    esp_log_set_vprintf(cdc_log_vprintf);

    ESP_LOGI(TAG, "USB Composite ready - FATFS at /spiflash, CDC+MSC active");
}

void usb_msc_switch_to_usb(void)
{
    if (storage_hdl) {
        tinyusb_msc_set_storage_mount_point(storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
        ESP_LOGI(TAG, "Switched to USB mode - PC can access storage");
    }
}

void usb_msc_switch_to_app(void)
{
    if (storage_hdl) {
        tinyusb_msc_set_storage_mount_point(storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_APP);
        ESP_LOGI(TAG, "Switched to APP mode - ESP32 can read files");
    }
}
