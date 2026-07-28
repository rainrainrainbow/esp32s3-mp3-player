/*
 * tft_driver.c - SPI TFT LCD driver for ST7789
 * Matched to working ILI9341 init sequence from esp32s3-mp3-i2c-player
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "config.h"
#include "tft_driver.h"
#include "image_decoder.h"

static const char *TAG = "TFT";

static spi_device_handle_t spi_dev = NULL;
static bool tft_initialized = false;

// DMA buffer for pixel data
#define DMA_BUFFER_SIZE 2048
static uint16_t *dma_buffer = NULL;

// LCD commands
#define CMD_SWRESET    0x01
#define CMD_SLPOUT     0x11
#define CMD_DISPON     0x29
#define CMD_CASET      0x2A
#define CMD_RASET      0x2B
#define CMD_RAMWR      0x2C
#define CMD_MADCTL     0x36
#define CMD_COLMOD     0x3A

// Backlight PWM config
#define LEDC_TIMER      LEDC_TIMER_1
#define LEDC_CHANNEL    LEDC_CHANNEL_1
#define LEDC_DUTY_RES   LEDC_TIMER_10_BIT
#define LEDC_FREQ       5000

static void tft_send_cmd(uint8_t cmd)
{
    if (!spi_dev) return;
    
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    gpio_set_level(DISPLAY_DC_GPIO, 0);
    spi_device_transmit(spi_dev, &t);
}

static void tft_send_data(uint8_t *data, size_t len)
{
    if (!spi_dev || len == 0) return;
    
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    spi_device_transmit(spi_dev, &t);
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
    ESP_LOGI(TAG, "=== TFT INIT ===");
    ESP_LOGI(TAG, "Pins: DC=%d CS=%d CLK=%d MOSI=%d BLK=%d", 
             DISPLAY_DC_GPIO, DISPLAY_CS_GPIO, DISPLAY_CLK_GPIO, 
             DISPLAY_MOSI_GPIO, DISPLAY_BACKLIGHT_PIN);

    // Allocate DMA buffer
    dma_buffer = heap_caps_malloc(DMA_BUFFER_SIZE * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!dma_buffer) {
        ESP_LOGE(TAG, "DMA buffer alloc failed");
        return;
    }

    // Configure backlight PWM
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = DISPLAY_BACKLIGHT_PIN,
        .duty = 768,
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);

    // Configure control pins
    gpio_set_direction(DISPLAY_DC_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(DISPLAY_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DISPLAY_CS_GPIO, 1);
    gpio_set_level(DISPLAY_BACKLIGHT_PIN, 1);

    // Initialize SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = DISPLAY_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = DISPLAY_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DMA_BUFFER_SIZE * 2,
    };
    spi_bus_initialize(DISPLAY_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    // Add SPI device - 40MHz like working version
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,  // SPI_MODE0
        .clock_speed_hz = 40 * 1000 * 1000,  // 40 MHz
        .spics_io_num = DISPLAY_CS_GPIO,
        .queue_size = 7,
    };
    spi_bus_add_device(DISPLAY_SPI_HOST, &dev_cfg, &spi_dev);
    ESP_LOGI(TAG, "SPI device added");

    // Init sequence - MATCH WORKING VERSION
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // SWRESET
    tft_send_cmd(CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // SLPOUT
    tft_send_cmd(CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // MADCTL = 0x08 (BGR only, no MX) - fix horizontal mirror
    tft_send_cmd(CMD_MADCTL);
    uint8_t madctl = 0x08;
    tft_send_data(&madctl, 1);
    ESP_LOGI(TAG, "MADCTL = 0x%02X", madctl);
    
    // COLMOD = 0x55 (16-bit)
    tft_send_cmd(CMD_COLMOD);
    uint8_t colmod = 0x55;
    tft_send_data(&colmod, 1);
    
    // DISPON
    tft_send_cmd(CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(50));

    tft_initialized = true;
    ESP_LOGI(TAG, "=== TFT INIT DONE ===");
    
    // Test fill
    tft_fill_screen(0x0000);
}

void tft_set_backlight(uint8_t brightness)
{
    uint32_t duty = (brightness * 1023) / 255;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

void tft_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (percent * 1023) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

void tft_fill_screen(uint16_t color)
{
    if (!tft_initialized || !dma_buffer) return;
    
    tft_set_addr_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    
    // Fill buffer with color (byte-swapped for SPI)
    for (int i = 0; i < DMA_BUFFER_SIZE; i++) {
        dma_buffer[i] = ((color >> 8) & 0xFF) | ((color & 0xFF) << 8);
    }
    
    size_t total = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    
    while (total > 0) {
        size_t chunk = (total > DMA_BUFFER_SIZE) ? DMA_BUFFER_SIZE : total;
        spi_transaction_t t = {
            .length = chunk * 16,
            .tx_buffer = dma_buffer,
        };
        spi_device_transmit(spi_dev, &t);
        total -= chunk;
    }
}

void tft_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (!tft_initialized || !dma_buffer) return;
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    tft_set_addr_window(x, y, x, y);
    
    dma_buffer[0] = ((color >> 8) & 0xFF) | ((color & 0xFF) << 8);
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = dma_buffer,
    };
    spi_device_transmit(spi_dev, &t);
}

void tft_show_image_file(const char *filepath)
{
    if (!tft_initialized || !dma_buffer) return;
    
    decoded_image_t img = {0};
    if (!decode_image_file(filepath, DISPLAY_WIDTH, DISPLAY_HEIGHT, &img)) {
        ESP_LOGE(TAG, "Decode failed: %s", filepath);
        return;
    }

    tft_set_addr_window(0, 0, img.width - 1, img.height - 1);
    
    size_t total = (size_t)img.width * img.height;
    uint16_t *src = img.pixels;
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    
    while (total > 0) {
        size_t chunk = (total > DMA_BUFFER_SIZE) ? DMA_BUFFER_SIZE : total;
        
        // Byte-swap into DMA buffer
        for (size_t i = 0; i < chunk; i++) {
            dma_buffer[i] = ((src[i] >> 8) & 0xFF) | ((src[i] & 0xFF) << 8);
        }
        
        spi_transaction_t t = {
            .length = chunk * 16,
            .tx_buffer = dma_buffer,
        };
        spi_device_transmit(spi_dev, &t);
        
        src += chunk;
        total -= chunk;
    }

    free_decoded_image(&img);
}

void tft_show_welcome(void)
{
    if (!tft_initialized) return;
    tft_fill_screen(0x001F);  // Blue
}

void tft_show_rgb565(const uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (!tft_initialized || !dma_buffer || !pixels) return;
    if (width == 0 || height == 0) return;

    tft_set_addr_window(0, 0, width - 1, height - 1);

    size_t total = (size_t)width * height;
    const uint16_t *src = pixels;
    gpio_set_level(DISPLAY_DC_GPIO, 1);

    while (total > 0) {
        size_t chunk = (total > DMA_BUFFER_SIZE) ? DMA_BUFFER_SIZE : total;

        // Byte-swap into DMA buffer for SPI big-endian
        for (size_t i = 0; i < chunk; i++) {
            dma_buffer[i] = ((src[i] >> 8) & 0xFF) | ((src[i] & 0xFF) << 8);
        }

        spi_transaction_t t = {
            .length = chunk * 16,
            .tx_buffer = dma_buffer,
        };
        spi_device_transmit(spi_dev, &t);

        src += chunk;
        total -= chunk;
    }
}
