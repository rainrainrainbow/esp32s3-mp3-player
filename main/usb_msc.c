/*
 * usb_msc.c - USB Composite Device (CDC ACM + MSC U盘)
 * CDC ACM充当串口代替USB Serial/JTAG，MSC通过SPI Flash FATFS暴露为U盘
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"
#include "config.h"
#include "usb_msc.h"

static const char *TAG = "USB_MSC";

tinyusb_msc_storage_handle_t storage_hdl = NULL;

/* CDC RX buffer */
static uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];

/**
 * @brief CDC device RX callback
 */
void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read(itf, rx_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret == ESP_OK && rx_size > 0) {
        // Echo back for now (simple serial monitor)
        tinyusb_cdcacm_write_queue(itf, rx_buf, rx_size);
        tinyusb_cdcacm_write_flush(itf, 0);
    }
}

void usb_msc_init(void)
{
    ESP_LOGI(TAG, "Initializing USB Composite (CDC + MSC)");

    // Find FATFS partition
    const esp_partition_t *data_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition");
        return;
    }
    ESP_LOGI(TAG, "Found partition: %s, size: %ld bytes",
             data_partition->label, (long)data_partition->size);

    // Mount wear levelling
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t ret = wl_mount(data_partition, &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount wear levelling: %s", esp_err_to_name(ret));
        return;
    }

    // Configure TinyUSB MSC storage
    const tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,  // Expose to USB by default
        .medium.wl_handle = wl_handle,
        .fat_fs = {
            .base_path = NULL,
            .config.max_files = 5,
            .format_flags = 0,
        },
    };

    ret = tinyusb_msc_new_storage_spiflash(&storage_cfg, &storage_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create storage: %s", esp_err_to_name(ret));
        return;
    }

    // Install TinyUSB driver (default config handles composite descriptors)
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize CDC ACM (virtual serial port)
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ret = tinyusb_cdcacm_init(&acm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init CDC ACM: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "USB Composite initialized: CDC ACM + MSC U盘");
    ESP_LOGI(TAG, "Connect USB to PC - you'll see a serial port AND a removable disk");
}
