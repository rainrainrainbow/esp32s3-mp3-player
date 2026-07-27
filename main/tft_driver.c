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
#define DMA_BUFFER_SIZE 2048  // Reduced for stability
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
    if (!spi_dev) {
        ESP_LOGE(TAG, "SPI dev NULL, skip cmd 0x%02X", cmd);
        return;
    }
    
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    gpio_set_level(DISPLAY_DC_GPIO, 0);
    esp_err_t ret = spi_device_transmit(spi_dev, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI cmd 0x%02X failed: %s", cmd, esp_err_to_name(ret));
    }
}

static void tft_send_data(uint8_t *data, size_t len)
{
    if (!spi_dev || len == 0) return;
    
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    esp_err_t ret = spi_device_transmit(spi_dev, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI data failed: %s", esp_err_to_name(ret));
    }
}

/* Byte-swap RGB565: ESP32 LE -> ST7789 BE */
static inline uint16_t byte_swap_16(uint16_t val)
{
    return (val >> 8) | (val << 8);
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
    ESP_LOGI(TAG, "=== TFT INIT START ===");
    ESP_LOGI(TAG, "Display: %dx%d logical", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    ESP_LOGI(TAG, "Pins: DC=%d CS=%d CLK=%d MOSI=%d BLK=%d", 
             DISPLAY_DC_GPIO, DISPLAY_CS_GPIO, DISPLAY_CLK_GPIO, 
             DISPLAY_MOSI_GPIO, DISPLAY_BACKLIGHT_PIN);
    ESP_LOGI(TAG, "SPI Host: %d, Mode: %d", DISPLAY_SPI_HOST, DISPLAY_SPI_MODE);

    // Allocate DMA buffer
    ESP_LOGI(TAG, "Allocating DMA buffer: %d bytes", DMA_BUFFER_SIZE * 2);
    dma_buffer = heap_caps_malloc(DMA_BUFFER_SIZE * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!dma_buffer) {
        ESP_LOGE(TAG, "FATAL: DMA buffer allocation failed!");
        return;
    }
    ESP_LOGI(TAG, "DMA buffer OK: %p", dma_buffer);

    // Configure backlight with PWM
    ESP_LOGI(TAG, "Configuring backlight PWM...");
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    ESP_LOGI(TAG, "LEDC timer: %s", esp_err_to_name(ret));

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
    ESP_LOGI(TAG, "LEDC channel: %s", esp_err_to_name(ret));

    // Configure control pins
    ESP_LOGI(TAG, "Configuring GPIO pins...");
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
        ESP_LOGE(TAG, "FATAL: SPI bus init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPI bus initialized");

    // Add SPI device
    ESP_LOGI(TAG, "Adding SPI device...");
    spi_device_interface_config_t dev_cfg = {
        .mode = DISPLAY_SPI_MODE,
        .clock_speed_hz = 10 * 1000 * 1000, // 10 MHz for stability
        .spics_io_num = DISPLAY_CS_GPIO,
        .queue_size = 7,
    };

    ret = spi_bus_add_device(DISPLAY_SPI_HOST, &dev_cfg, &spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: SPI add device failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPI device added, handle=%p", spi_dev);

    // Software reset
    ESP_LOGI(TAG, "Sending SWRESET...");
    tft_send_cmd(CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Exit sleep mode
    ESP_LOGI(TAG, "Sending SLPOUT...");
    tft_send_cmd(CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Set color mode to 16-bit RGB565
    ESP_LOGI(TAG, "Setting COLMOD=0x55 (16-bit)...");
    tft_send_cmd(CMD_COLMOD);
    uint8_t colmod = 0x55;
    tft_send_data(&colmod, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Set memory access control (orientation)
    ESP_LOGI(TAG, "Setting MADCTL...");
    tft_send_cmd(CMD_MADCTL);
    uint8_t madctl = 0;
    if (DISPLAY_SWAP_XY) madctl |= MADCTL_MV;
    if (DISPLAY_MIRROR_X) madctl |= MADCTL_MX;
    if (DISPLAY_MIRROR_Y) madctl |= MADCTL_MY;
    madctl |= MADCTL_BGR;
    ESP_LOGI(TAG, "MADCTL = 0x%02X", madctl);
    tft_send_data(&madctl, 1);

    // Enable inversion
    ESP_LOGI(TAG, "Sending INVON...");
    tft_send_cmd(CMD_INVON);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Normal display mode
    ESP_LOGI(TAG, "Sending NORON...");
    tft_send_cmd(CMD_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Display ON
    ESP_LOGI(TAG, "Sending DISPON...");
    tft_send_cmd(CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));

    tft_initialized = true;
    ESP_LOGI(TAG, "=== TFT INIT COMPLETE ===");
    
    // Test: fill with red to verify display works
    ESP_LOGI(TAG, "TEST: Filling screen with RED...");
    tft_set_addr_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    for (int i = 0; i < DMA_BUFFER_SIZE; i++) {
        dma_buffer[i] = byte_swap_16(0xF800);  // Red
    }
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    size_t total = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    while (total > 0) {
        size_t chunk = (total > DMA_BUFFER_SIZE) ? DMA_BUFFER_SIZE : total;
        spi_transaction_t t = {
            .length = chunk * 16,
            .tx_buffer = dma_buffer,
        };
        ret = spi_device_transmit(spi_dev, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TEST fill failed: %s", esp_err_to_name(ret));
            break;
        }
        total -= chunk;
    }
    ESP_LOGI(TAG, "TEST: RED screen should be visible now!");
    vTaskDelay(pdMS_TO_TICKS(1000));  // Show red for 1 second
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
    ESP_LOGI(TAG, "Brightness: %d%% (duty=%lu)", percent, (unsigned long)duty);
}

void tft_fill_screen(uint16_t color)
{
    if (!tft_initialized || !dma_buffer) {
        ESP_LOGE(TAG, "Cannot fill: init=%d dma=%p", tft_initialized, dma_buffer);
        return;
    }
    
    ESP_LOGI(TAG, "Fill screen 0x%04X", color);
    tft_set_addr_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    
    for (int i = 0; i < DMA_BUFFER_SIZE; i++) {
        dma_buffer[i] = byte_swap_16(color);
    }
    
    size_t total = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    
    while (total > 0) {
        size_t chunk = (total > DMA_BUFFER_SIZE) ? DMA_BUFFER_SIZE : total;
        spi_transaction_t t = {
            .length = chunk * 16,
            .tx_buffer = dma_buffer,
        };
        esp_err_t ret = spi_device_transmit(spi_dev, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Fill failed: %s", esp_err_to_name(ret));
            break;
        }
        total -= chunk;
    }
}

void tft_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (!tft_initialized || !dma_buffer) return;
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    tft_set_addr_window(x, y, x, y);
    
    dma_buffer[0] = byte_swap_16(color);
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = dma_buffer,
    };
    spi_device_transmit(spi_dev, &t);
}

void tft_show_image_file(const char *filepath)
{
    if (!tft_initialized || !dma_buffer) {
        ESP_LOGE(TAG, "Cannot show image: init=%d dma=%p", tft_initialized, dma_buffer);
        return;
    }
    
    decoded_image_t img = {0};

    ESP_LOGI(TAG, "Decoding: %s", filepath);
    if (!decode_image_file(filepath, DISPLAY_WIDTH, DISPLAY_HEIGHT, &img)) {
        ESP_LOGE(TAG, "Decode failed: %s", filepath);
        return;
    }

    ESP_LOGI(TAG, "Display %s (%dx%d)", filepath, img.width, img.height);

    tft_set_addr_window(0, 0, img.width - 1, img.height - 1);
    
    // Send pixels via DMA
    size_t total = (size_t)img.width * img.height;
    uint16_t *src = img.pixels;
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    
    while (total > 0) {
        size_t chunk = (total > DMA_BUFFER_SIZE) ? DMA_BUFFER_SIZE : total;
        
        // Byte-swap into DMA buffer
        for (size_t i = 0; i < chunk; i++) {
            dma_buffer[i] = byte_swap_16(src[i]);
        }
        
        spi_transaction_t t = {
            .length = chunk * 16,
            .tx_buffer = dma_buffer,
        };
        esp_err_t ret = spi_device_transmit(spi_dev, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Image SPI failed: %s", esp_err_to_name(ret));
            break;
        }
        
        src += chunk;
        total -= chunk;
    }

    ESP_LOGI(TAG, "Image done");
    free_decoded_image(&img);
}

/* Welcome screen */
void tft_show_welcome(void)
{
    ESP_LOGI(TAG, "=== WELCOME SCREEN START ===");
    
    if (!tft_initialized) {
        ESP_LOGE(TAG, "TFT not initialized!");
        return;
    }
    if (!dma_buffer) {
        ESP_LOGE(TAG, "DMA buffer NULL!");
        return;
    }
    
    ESP_LOGI(TAG, "Filling BLUE...");
    tft_fill_screen(0x001F);  // Blue
    
    ESP_LOGI(TAG, "=== WELCOME SCREEN DONE ===");
}
