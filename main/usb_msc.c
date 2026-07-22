/*
 * USB composite device: CDC ACM + manually-owned MSC SPI Flash storage.
 *
 * Critical rule: FATFS can only have one owner.  CDC connection must NOT
 * automatically move the storage from APP to USB, otherwise /spiflash is
 * unmounted exactly while the serial monitor is attached.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"
#include "config.h"
#include "usb_msc.h"

static const char *TAG = "USB_MSC";
static tinyusb_msc_storage_handle_t storage_hdl = NULL;
static bool storage_is_app = false;

static const char *mount_name(tinyusb_msc_mount_point_t point)
{
    return point == TINYUSB_MSC_STORAGE_MOUNT_APP ? "APP" : "USB";
}

static const char *event_name(tinyusb_msc_event_id_t id)
{
    switch (id) {
    case TINYUSB_MSC_EVENT_MOUNT_START: return "MOUNT_START";
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE: return "MOUNT_COMPLETE";
    case TINYUSB_MSC_EVENT_MOUNT_FAILED: return "MOUNT_FAILED";
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED: return "FORMAT_REQUIRED";
    case TINYUSB_MSC_EVENT_FORMAT_FAILED: return "FORMAT_FAILED";
    default: return "UNKNOWN";
    }
}

static void storage_event_cb(tinyusb_msc_storage_handle_t handle,
                             tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;
    ESP_LOGI(TAG, "MSC event %s, target=%s",
             event_name(event->id), mount_name(event->mount_point));
}

/* Redirect ESP logs to TinyUSB CDC after CDC has been initialized. */
static int cdc_log_vprintf(const char *fmt, va_list args)
{
    char buf[512];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        size_t send_len = (len < (int)sizeof(buf)) ? (size_t)len : sizeof(buf) - 1;
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t *)buf, send_len);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
    return len;
}

void usb_msc_init(void)
{
    ESP_LOGI(TAG, "Initializing USB CDC + manually-owned MSC");

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
        STORAGE_PARTITION_LABEL);
    if (!part) {
        ESP_LOGE(TAG, "FAT partition '%s' not found", STORAGE_PARTITION_LABEL);
        return;
    }

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t ret = wl_mount(part, &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Disable esp_tinyusb's automatic APP<->USB switching on cable events. */
    const tinyusb_msc_driver_config_t msc_driver_cfg = {
        .user_flags = { .auto_mount_off = 1 },
        .callback = storage_event_cb,
        .callback_arg = NULL,
    };
    ret = tinyusb_msc_install_driver(&msc_driver_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MSC driver install failed: %s", esp_err_to_name(ret));
        return;
    }

    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium.wl_handle = wl_handle,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = STORAGE_MOUNT_POINT,
            .config = {
                .format_if_mount_failed = false,
                .max_files = STORAGE_MAX_FILES,
                .allocation_unit_size = 4096,
            },
            .do_not_format = true,
            .format_flags = 0,
        },
    };
    ret = tinyusb_msc_new_storage_spiflash(&storage_cfg, &storage_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create MSC storage failed: %s", esp_err_to_name(ret));
        return;
    }
    storage_is_app = true;

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(ret));
        return;
    }

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ret = tinyusb_cdcacm_init(&acm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CDC init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_log_set_vprintf(cdc_log_vprintf);
    ESP_LOGI(TAG, "USB ready; storage owner=APP, VFS=%s", STORAGE_MOUNT_POINT);
    ESP_LOGI(TAG, "MSC auto-switch disabled; long-press GPIO0 to expose/eject disk");
}

bool usb_msc_switch_to_usb(void)
{
    if (!storage_hdl) return false;
    esp_err_t ret = tinyusb_msc_set_storage_mount_point(
        storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Switch to USB failed: %s", esp_err_to_name(ret));
        return false;
    }
    storage_is_app = false;
    ESP_LOGW(TAG, "Storage owner=USB; ESP32 file access disabled");
    return true;
}

bool usb_msc_switch_to_app(void)
{
    if (!storage_hdl) return false;
    esp_err_t ret = tinyusb_msc_set_storage_mount_point(
        storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Switch to APP failed: %s", esp_err_to_name(ret));
        return false;
    }
    storage_is_app = true;
    ESP_LOGI(TAG, "Storage owner=APP; %s available", STORAGE_MOUNT_POINT);
    return true;
}

bool usb_msc_is_app_mode(void)
{
    return storage_is_app;
}
