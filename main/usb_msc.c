/*
 * usb_msc.c - USB MSC stub (TinyUSB not available in IDF v5.3 container)
 * TODO: Install esp_tinyusb via component manager for full USB MSC support
 */
#include <stdio.h>
#include "esp_log.h"
#include "config.h"
#include "usb_msc.h"

static const char *TAG = "USB_MSC";

void usb_msc_init(void)
{
    ESP_LOGW(TAG, "USB MSC not available - install esp_tinyusb component");
    ESP_LOGW(TAG, "Add to idf_component.yml: espressif/esp_tinyusb: ^1.0");
}
