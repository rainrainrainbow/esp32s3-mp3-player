/*
 * tft_driver.c - SPI TFT LCD driver for ST7789 with DMA optimization
 * Uses ESP-IDF SPI master driver with DMA for bulk transfers
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

// DMA buffer for byte-swapped pixel data
#define DMA_BUFFER_SIZE 4096
static uint16_t *dma_buffer = NULL;

// LCD commands
#define CMD_NOP        0x00
#define CMD_SWRESET    0x01
#define CMD_SLPIN      0x10
#define CMD_SLPOUT     0x11
#define CMD_NORON      0x13
#define CMD_INVON      0x21
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

// Backlight PWM config
#define LEDC_TIMER      LEDC_TIMER_1
#define LEDC_CHANNEL    LEDC_CHANNEL_1
#define LEDC_DUTY_RES   LEDC_TIMER_10_BIT
#define LEDC_FREQ       5000

static void tft_send_cmd(uint8_t cmd)
{
    if (!tft_initialized || !spi_dev) {
        ESP_LOGE(TAG, "TFT not initialized, skipping cmd 0x%02X", cmd);
        return;
    }
    
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    gpio_set_level(DISPLAY_DC_GPIO, 0);
    esp_err_t ret = spi_device_transmit(spi_dev, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI cmd transmit failed: %s", esp_err_to_name(ret));
    }
}

static void tft_send_data(uint8_t *data, size_t len)
{
    if (!tft_initialized || !spi_dev || len == 0) return;
    
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    esp_err_t ret = spi_device_transmit(spi_dev, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI data transmit failed: %s", esp_err_to_name(ret));
    }
}

/* Byte-swap RGB565: ESP32 LE -> ST7789 BE */
static inline uint16_t byte_swap_16(uint16_t val)
{
    return (val >> 8) | (val << 8);
}

/* Send 16-bit pixel data with DMA-optimized byte-swap */
static void tft_send_pixels_dma(uint16_t *pixels, size_t count)
{
    if (!tft_initialized || !spi_dev || !dma_buffer || count == 0) return;
    
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    
    size_t remaining = count;
    uint16_t *src = pixels;
    
    while (remaining > 0) {
        size_t chunk = (remaining > DMA_BUFFER_SIZE) ? DMA_BUFFER_SIZE : remaining;
        
        // Byte-swap into DMA buffer
        for (size_t i = 0; i < chunk; i++) {
            dma_buffer[i] = byte_swap_16(src[i]);
        }
        
        // Send via SPI with DMA
        spi_transaction_t t = {
            .length = chunk * 16,
            .tx_buffer = dma_buffer,
        };
        
        esp_err_t ret = spi_device_transmit(spi_dev, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI DMA transmit failed: %s", esp_err_to_name(ret));
            break;
        }
        
        src += chunk;
        remaining -= chunk;
    }
}

static void tft_set_addr_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    if (!tft_initialized) return;
    
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
    ESP_LOGI(TAG, "Initializing ST7789 display (%dx%d logical)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    ESP_LOGI(TAG, "Pins: DC=%d CS=%d CLK=%d MOSI=%d BLK=%d", 
             DISPLAY_DC_GPIO, DISPLAY_CS_GPIO, DISPLAY_CLK_GPIO, 
             DISPLAY_MOSI_GPIO, DISPLAY_BACKLIGHT_PIN);
    ESP_LOGI(TAG, "SPI Host: %d, Mode: %d", DISPLAY_SPI_HOST, DISPLAY_SPI_MODE);

    // Allocate DMA buffer
    dma_buffer = heap_caps_malloc(DMA_BUFFER_SIZE * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!dma_buffer) {
        ESP_LOGE(TAG, "Failed to allocate DMA buffer");
        return;
    }
    ESP_LOGI(TAG, "DMA buffer allocated: %d bytes", DMA_BUFFER_SIZE * 2);

    // Configure backlight with PWM
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(ret));
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = DISPLAY_BACKLIGHT_PIN,
        .duty = 768, // 75% brightness
        .hpoint = 0,
    };
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "Backlight initialized at 75%%");

    // Configure control pins
    gpio_set_direction(DISPLAY_DC_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(DISPLAY_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DISPLAY_CS_GPIO, 1);

    // Initialize SPI bus
    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = DISPLAY_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = DISPLAY_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DMA_BUFFER_SIZE * 2,
    };

    ret = spi_bus_initialize(DISPLAY_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus initialize failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPI bus initialized with DMA");

    // Add SPI device
    ESP_LOGI(TAG, "Adding SPI device...");
    spi_device_interface_config_t dev_cfg = {
        .mode = DISPLAY_SPI_MODE,
        .clock_speed_hz = 20 * 1000 * 1000, // 20 MHz (DMA can handle higher speed)
        .spics_io_num = DISPLAY_CS_GPIO,
        .queue_size = 7,
    };

    ret = spi_bus_add_device(DISPLAY_SPI_HOST, &dev_cfg, &spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus add device failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPI device added, handle=%p", spi_dev);

    // Software reset
    ESP_LOGI(TAG, "Sending software reset...");
    tft_send_cmd(CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Exit sleep mode
    ESP_LOGI(TAG, "Exiting sleep mode...");
    tft_send_cmd(CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Set color mode to 16-bit RGB565
    ESP_LOGI(TAG, "Setting color mode to 16-bit RGB565...");
    tft_send_cmd(CMD_COLMOD);
    uint8_t colmod = 0x55;
    tft_send_data(&colmod, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Set memory access control (orientation)
    ESP_LOGI(TAG, "Setting orientation: SWAP_XY=%d MIRROR_X=%d MIRROR_Y=%d",
             DISPLAY_SWAP_XY, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    tft_send_cmd(CMD_MADCTL);
    uint8_t madctl = 0;
    if (DISPLAY_SWAP_XY) madctl |= MADCTL_MV;
    if (DISPLAY_MIRROR_X) madctl |= MADCTL_MX;
    if (DISPLAY_MIRROR_Y) madctl |= MADCTL_MY;
    madctl |= MADCTL_BGR;
    ESP_LOGI(TAG, "MADCTL = 0x%02X", madctl);
    tft_send_data(&madctl, 1);

    // Enable inversion
    ESP_LOGI(TAG, "Enabling display inversion...");
    tft_send_cmd(CMD_INVON);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Normal display mode
    tft_send_cmd(CMD_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Display ON
    ESP_LOGI(TAG, "Turning display on...");
    tft_send_cmd(CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));

    tft_initialized = true;
    ESP_LOGI(TAG, "ST7789 initialization complete");
}

void tft_set_backlight(uint8_t brightness)
{
    if (brightness == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 768);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
    }
}

void tft_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (percent * 1023) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
    ESP_LOGI(TAG, "Brightness set to %d%% (duty=%lu)", percent, (unsigned long)duty);
}

void tft_fill_screen(uint16_t color)
{
    if (!tft_initialized) {
        ESP_LOGE(TAG, "TFT not initialized, cannot fill screen");
        return;
    }
    
    ESP_LOGI(TAG, "Filling screen with color 0x%04X", color);
    tft_set_addr_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    
    // Fill DMA buffer with color
    for (int i = 0; i < DMA_BUFFER_SIZE; i++) {
        dma_buffer[i] = byte_swap_16(color);
    }
    
    // Send in chunks
    size_t total_pixels = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    
    while (total_pixels > 0) {
        size_t chunk = (total_pixels > DMA_BUFFER_SIZE) ? DMA_BUFFER_SIZE : total_pixels;
        spi_transaction_t t = {
            .length = chunk * 16,
            .tx_buffer = dma_buffer,
        };
        esp_err_t ret = spi_device_transmit(spi_dev, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI fill failed: %s", esp_err_to_name(ret));
            break;
        }
        total_pixels -= chunk;
    }
    
    ESP_LOGI(TAG, "Screen fill complete");
}

void tft_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (!tft_initialized) return;
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    tft_set_addr_window(x, y, x, y);
    
    uint16_t swapped = byte_swap_16(color);
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = &swapped,
    };
    spi_device_transmit(spi_dev, &t);
}

void tft_show_image_file(const char *filepath)
{
    if (!tft_initialized) {
        ESP_LOGE(TAG, "TFT not initialized, cannot show image");
        return;
    }
    
    decoded_image_t img = {0};

    ESP_LOGI(TAG, "Decoding image: %s", filepath);
    if (!decode_image_file(filepath, DISPLAY_WIDTH, DISPLAY_HEIGHT, &img)) {
        ESP_LOGE(TAG, "Failed to decode image: %s", filepath);
        return;
    }

    ESP_LOGI(TAG, "Displaying %s (%dx%d)", filepath, img.width, img.height);

    tft_set_addr_window(0, 0, img.width - 1, img.height - 1);
    tft_send_pixels_dma(img.pixels, (size_t)img.width * img.height);

    ESP_LOGI(TAG, "Image display complete");
    free_decoded_image(&img);
}

/* Fast welcome screen using DMA bulk fills */
void tft_show_welcome(void)
{
    if (!tft_initialized) {
        ESP_LOGE(TAG, "TFT not initialized, cannot show welcome");
        return;
    }
    
    ESP_LOGI(TAG, "Showing welcome screen...");
    
    // Blue background
    tft_fill_screen(0x001F);
    
    // Green top bar (20 pixels high)
    tft_set_addr_window(0, 0, DISPLAY_WIDTH - 1, 19);
    for (int i = 0; i < DMA_BUFFER_SIZE; i++) {
        dma_buffer[i] = byte_swap_16(0x07E0);
    }
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    for (int y = 0; y < 20; y += (DMA_BUFFER_SIZE / DISPLAY_WIDTH)) {
        size_t rows = (20 - y > (DMA_BUFFER_SIZE / DISPLAY_WIDTH)) ? (DMA_BUFFER_SIZE / DISPLAY_WIDTH) : (20 - y);
        spi_transaction_t t = {
            .length = rows * DISPLAY_WIDTH * 16,
            .tx_buffer = dma_buffer,
        };
        spi_device_transmit(spi_dev, &t);
    }
    
    // Red bottom bar (20 pixels high)
    tft_set_addr_window(0, DISPLAY_HEIGHT - 20, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    for (int i = 0; i < DMA_BUFFER_SIZE; i++) {
        dma_buffer[i] = byte_swap_16(0xF800);
    }
    for (int y = 0; y < 20; y += (DMA_BUFFER_SIZE / DISPLAY_WIDTH)) {
        size_t rows = (20 - y > (DMA_BUFFER_SIZE / DISPLAY_WIDTH)) ? (DMA_BUFFER_SIZE / DISPLAY_WIDTH) : (20 - y);
        spi_transaction_t t = {
            .length = rows * DISPLAY_WIDTH * 16,
            .tx_buffer = dma_buffer,
        };
        spi_device_transmit(spi_dev, &t);
    }
    
    ESP_LOGI(TAG, "Welcome screen displayed");
}
