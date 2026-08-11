/*
 * lvgl_port.c - LVGL display flush and touch input port (v8.3)
 * Uses existing tft_driver.c for SPI LCD, adds LVGL flush_cb and
 * touch input via FT5x06 (I2C capacitive touch).
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "config.h"
#include "tft_driver.h"
#include "touch_driver.h"
#include "lvgl_port.h"

static const char *TAG = "LVGL_PORT";

static SemaphoreHandle_t lvgl_mutex = NULL;

/* LVGL display buffer - use PSRAM for large buffer */
#define LVGL_BUF_SIZE (DISPLAY_WIDTH * 40)  /* 40 lines partial buffer */
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;

/* LVGL display driver (v8.3 style) */
static lv_disp_drv_t disp_drv;
static lv_disp_t *disp = NULL;
static lv_disp_draw_buf_t draw_buf;

/* LVGL input device */
static lv_indev_drv_t indev_drv;
static lv_indev_t *touch_indev = NULL;

/*
 * Display flush callback - called by LVGL when a buffer is ready
 */
static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint16_t w = area->x2 - area->x1 + 1;
    uint16_t h = area->y2 - area->y1 + 1;

    /* Send the partial buffer to the display */
    tft_show_rgb565_area((const uint16_t *)color_p, w, h, area->x1, area->y1);

    /* Inform LVGL that flushing is done */
    lv_disp_flush_ready(drv);
}

/*
 * Touch input read callback - called by LVGL to poll touch
 */
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    touch_point_t pt;
    if (touch_driver_get_point(&pt)) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

/*
 * LVGL tick task - calls lv_tick_inc periodically
 */
static void lvgl_tick_task(void *arg)
{
    while (1) {
        lv_tick_inc(5);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/*
 * LVGL timer handler task - calls lv_timer_handler periodically
 */
static void lvgl_timer_task(void *arg)
{
    while (1) {
        if (lvgl_port_lock(100)) {
            lv_timer_handler();
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void lvgl_port_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL v8.3...");

    /* Create mutex for LVGL thread safety */
    lvgl_mutex = xSemaphoreCreateMutex();
    if (!lvgl_mutex) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return;
    }

    /* Initialize LVGL core */
    lv_init();

    /* Allocate display buffers from PSRAM */
    buf1 = heap_caps_malloc(LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    buf2 = heap_caps_malloc(LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL display buffers");
        return;
    }

    /* Initialize draw buffer */
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LVGL_BUF_SIZE);

    /* Initialize display driver */
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISPLAY_WIDTH;
    disp_drv.ver_res = DISPLAY_HEIGHT;
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp = lv_disp_drv_register(&disp_drv);

    if (!disp) {
        ESP_LOGE(TAG, "Failed to register LVGL display driver");
        return;
    }

    /* Initialize touch driver */
    if (touch_driver_init() == ESP_OK) {
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = touch_read_cb;
        touch_indev = lv_indev_drv_register(&indev_drv);
        ESP_LOGI(TAG, "Touch input enabled (FT5x06)");
    } else {
        ESP_LOGW(TAG, "Touch driver init failed - touch disabled");
    }

    /* Create LVGL tasks */
    xTaskCreatePinnedToCore(lvgl_tick_task, "lvgl_tick", 2048, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(lvgl_timer_task, "lvgl_timer", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "LVGL initialized: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

uint32_t lvgl_port_tick_get(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool lvgl_port_lock(int timeout_ms)
{
    if (!lvgl_mutex) return false;
    return xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    if (lvgl_mutex) xSemaphoreGive(lvgl_mutex);
}