/*
 * tft_driver.c - SPI TFT LCD driver for ST7789 (240x320)
 * Uses ESP-IDF SPI master driver
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "config.h"
#include "tft_driver.h"
#include "image_decoder.h"

static const char *TAG = "TFT";

static spi_device_handle_t spi_dev;
static uint16_t *fb = NULL; // framebuffer for display

// LCD commands
#define CMD_SWRESET    0x01
#define CMD_SLPOUT     0x11
#define CMD_NORON      0x13
#define CMD_INVOFF     0x20
#define CMD_DISPON     0x29
#define CMD_CASET      0x2A
#define CMD_RASET      0x2B
#define CMD_RAMWR      0x2C
#define CMD_MADCTL     0x36
#define CMD_COLMOD     0x3A

// MADCTL bits
#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_ML  0x10
#define MADCTL_BGR 0x08
#define MADCTL_MH  0x04

static void tft_send_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .flags = SPI_TRANS_CS_KEEP_ACTIVE,
    };
    gpio_set_level(DISPLAY_DC_GPIO, 0);
    spi_device_transmit(spi_dev, &t);
}

static void tft_send_data(uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t chunk = (len > 4096) ? 4096 : len;
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = data,
        };
        gpio_set_level(DISPLAY_DC_GPIO, 1);
        spi_device_transmit(spi_dev, &t);
        data += chunk;
        len -= chunk;
    }
}

static void tft_send_data16(uint16_t *data, size_t len)
{
    while (len > 0) {
        size_t chunk = (len > 2048) ? 2048 : len;
        spi_transaction_t t = {
            .length = chunk * 16,
            .tx_buffer = data,
        };
        gpio_set_level(DISPLAY_DC_GPIO, 1);
        spi_device_transmit(spi_dev, &t);
        data += chunk;
        len -= chunk;
    }
}

static void tft_set_addr_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t data[4];
    tft_send_cmd(CMD_CASET);
    data[0] = (x1 >> 8) & 0xFF;
    data[1] = x1 & 0xFF;
    data[2] = (x2 >> 8) & 0xFF;
    data[3] = x2 & 0xFF;
    tft_send_data(data, 4);

    tft_send_cmd(CMD_RASET);
    data[0] = (y1 >> 8) & 0xFF;
    data[1] = y1 & 0xFF;
    data[2] = (y2 >> 8) & 0xFF;
    data[3] = y2 & 0xFF;
    tft_send_data(data, 4);

    tft_send_cmd(CMD_RAMWR);
}

void tft_init(void)
{
    ESP_LOGI(TAG, "Initializing TFT display");

    // Configure backlight
    gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT ? 0 : 1);

    // Configure control pins
    gpio_set_direction(DISPLAY_DC_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(DISPLAY_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DISPLAY_CS_GPIO, 1);

    // Reset
    if (DISPLAY_RST_GPIO != GPIO_NUM_NC) {
        gpio_set_direction(DISPLAY_RST_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(DISPLAY_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(DISPLAY_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // SPI bus configuration
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = DISPLAY_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = DISPLAY_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32768,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .mode = DISPLAY_SPI_MODE,
        .clock_speed_hz = 80 * 1000 * 1000, // 80MHz
        .spics_io_num = DISPLAY_CS_GPIO,
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(spi_bus_add_device(DISPLAY_SPI_HOST, &dev_cfg, &spi_dev));

    // Initialize LCD
    tft_send_cmd(CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Set color mode to 16-bit
    tft_send_cmd(CMD_COLMOD);
    uint8_t colmod = 0x55; // 16-bit RGB565
    tft_send_data(&colmod, 1);

    // Set orientation
    tft_send_cmd(CMD_MADCTL);
    uint8_t madctl = 0;
    if (DISPLAY_SWAP_XY) madctl |= MADCTL_MV;
    if (DISPLAY_MIRROR_X) madctl |= MADCTL_MX;
    if (DISPLAY_MIRROR_Y) madctl |= MADCTL_MY;
    madctl |= MADCTL_BGR; // BGR order for ST7789
    tft_send_data(&madctl, 1);

    tft_send_cmd(CMD_NORON);
    tft_send_cmd(CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));
    tft_send_cmd(CMD_DISPON);

    // Allocate framebuffer
    fb = heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * 2, MALLOC_CAP_DMA);
    if (fb) {
        ESP_LOGI(TAG, "Framebuffer allocated: %d bytes", DISPLAY_WIDTH * DISPLAY_HEIGHT * 2);
    } else {
        ESP_LOGW(TAG, "Cannot allocate framebuffer, using direct write");
    }

    tft_fill_screen(0x0000); // black
    ESP_LOGI(TAG, "TFT initialized");
}

void tft_set_backlight(uint8_t brightness)
{
    // Simple PWM or on/off control
    bool level = DISPLAY_BACKLIGHT_OUTPUT_INVERT ? (brightness == 0) : (brightness > 0);
    gpio_set_level(DISPLAY_BACKLIGHT_PIN, level);
}

void tft_fill_screen(uint16_t color)
{
    tft_set_addr_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    size_t pixels = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    // Use a small buffer for filling
    uint16_t fill_buf[256];
    for (int i = 0; i < 256; i++) fill_buf[i] = color;
    for (size_t i = 0; i < pixels; i += 256) {
        size_t n = (pixels - i < 256) ? (pixels - i) : 256;
        tft_send_data16(fill_buf, n);
    }
}

void tft_draw_jpeg(const uint8_t *jpeg_data, size_t len, uint16_t x, uint16_t y)
{
    if (!fb) return;
    uint16_t width, height;
    if (jpeg_decode_to_rgb565_stub(jpeg_data, len, fb, DISPLAY_WIDTH * DISPLAY_HEIGHT * 2, &width, &height)) {
        tft_set_addr_window(x, y, x + width - 1, y + height - 1);
        tft_send_data16(fb, width * height);
    }
}

void tft_draw_bmp(const uint8_t *bmp_data, size_t len)
{
    if (!fb) return;
    uint16_t width, height;
    if (bmp_decode_to_rgb565_stub(bmp_data, len, fb, DISPLAY_WIDTH * DISPLAY_HEIGHT * 2, &width, &height)) {
        tft_set_addr_window(0, 0, width - 1, height - 1);
        tft_send_data16(fb, width * height);
    }
}

void tft_show_image_file(const char *filepath)
{
    if (!fb) {
        ESP_LOGE(TAG, "No framebuffer");
        return;
    }
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", filepath);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return;
    }
    uint8_t *buf = malloc(size);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "OOM reading %s", filepath);
        return;
    }
    fread(buf, 1, size, f);
    fclose(f);

    // Detect format by magic bytes
    if (size >= 2 && buf[0] == 0xFF && buf[1] == 0xD8) {
        // JPEG
        tft_draw_jpeg(buf, size, 0, 0);
    } else if (size >= 2 && buf[0] == 'B' && buf[1] == 'M') {
        // BMP
        tft_draw_bmp(buf, size);
    } else {
        ESP_LOGE(TAG, "Unsupported image format: %s", filepath);
    }
    free(buf);
}