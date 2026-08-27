/*
 * touch_driver.c - FT5x06 I2C capacitive touch driver
 * I2C address: 0x38
 * Registers:
 *   0x02: Number of touch points (low 4 bits)
 *   0x03: Touch 1 X high byte
 *   0x04: Touch 1 X low byte | Y high (upper 4 bits)
 *   0x05: Touch 1 Y high byte
 *   0x06: Touch 1 Y low byte
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "config.h"
#include "touch_driver.h"

static const char *TAG = "TOUCH";

/* FT5x06 registers */
#define FT5X06_REG_NUM_TOUCHES  0x02
#define FT5X06_REG_TOUCH1_XH    0x03
#define FT5X06_REG_TOUCH1_XL    0x04
#define FT5X06_REG_TOUCH1_YH    0x05
#define FT5X06_REG_TOUCH1_YL    0x06

/* Native touch panel resolution (portrait 240x320) */
#define FT5X06_NATIVE_X  240
#define FT5X06_NATIVE_Y  320

static bool touch_initialized = false;
static touch_point_t last_point = {0, 0};

/*
 * Read a register from FT5x06
 */
static esp_err_t ft5x06_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    esp_err_t ret = i2c_master_write_read_device(
        TOUCH_I2C_PORT, TOUCH_I2C_ADDR, &reg, 1, data, len,
        pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FT5x06 read reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
    }
    return ret;
}

/*
 * Write a register to FT5x06
 */
static esp_err_t ft5x06_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    esp_err_t ret = i2c_master_write_to_device(
        TOUCH_I2C_PORT, TOUCH_I2C_ADDR, data, sizeof(data),
        pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FT5x06 write reg 0x%02X=0x%02X failed: %s",
                 reg, value, esp_err_to_name(ret));
    }
    return ret;
}

/*
 * Software reset of FT5x06
 */
static void ft5x06_reset(void)
{
    ESP_LOGI(TAG, "Resetting FT5x06...");

    /* Hardware reset if RST pin configured */
    if (TOUCH_RST_GPIO != GPIO_NUM_NC) {
        gpio_set_direction(TOUCH_RST_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(TOUCH_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(TOUCH_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Software reset via register 0x00 */
    ft5x06_write_reg(0x00, 0x01);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/*
 * Initialize FT5x06 touch controller
 */
esp_err_t touch_driver_init(void)
{
    ESP_LOGI(TAG, "Initializing FT5x06 touch at addr 0x%02X", TOUCH_I2C_ADDR);
    ESP_LOGI(TAG, "Touch shares I2C bus with ES8311: SDA=%d, SCL=%d, PORT=%d",
             TOUCH_I2C_SDA, TOUCH_I2C_SCL, TOUCH_I2C_PORT);

    /* I2C bus is already initialized by ES8311 codec (same port & pins).
     * Do NOT re-initialize here - just verify the bus is available. */
    esp_err_t ret = ESP_OK;
    (void)ret;

    /* Reset the touch controller */
    ft5x06_reset();

    /* Wait for FT5x06 to stabilize after reset */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Verify chip ID (register 0xA8 = FT5x06 chip ID, usually 0x06) */
    uint8_t chip_id = 0;
    if (ft5x06_read_reg(0xA8, &chip_id, 1) == ESP_OK) {
        ESP_LOGI(TAG, "FT5x06 chip ID: 0x%02X (expected 0x06)", chip_id);
        if (chip_id != 0x06 && chip_id != 0x00) {
            ESP_LOGW(TAG, "Unexpected chip ID - may not be FT5x06");
        }
    } else {
        ESP_LOGE(TAG, "Failed to read FT5x06 chip ID - I2C communication error!");
        ESP_LOGE(TAG, "Check wiring: SDA=%d, SCL=%d, ADDR=0x%02X",
                 TOUCH_I2C_SDA, TOUCH_I2C_SCL, TOUCH_I2C_ADDR);
        /* Continue anyway - some clones don't respond to chip ID read */
    }

    /* Read firmware version (registers 0xA6-0xA7) */
    uint8_t fw_ver[2] = {0};
    if (ft5x06_read_reg(0xA6, fw_ver, 2) == ESP_OK) {
        ESP_LOGI(TAG, "FT5x06 firmware: 0x%02X%02X", fw_ver[0], fw_ver[1]);
    }

    /* Configure interrupt mode (0x86 = polling mode) */
    ft5x06_write_reg(0x86, 0x01);

    /* Test read of touch data register */
    uint8_t test_data = 0;
    if (ft5x06_read_reg(FT5X06_REG_NUM_TOUCHES, &test_data, 1) == ESP_OK) {
        ESP_LOGI(TAG, "Touch data register readable: num_touches=0x%02X", test_data);
    } else {
        ESP_LOGE(TAG, "Cannot read touch data register - touch will not work!");
    }

    touch_initialized = true;
    ESP_LOGI(TAG, "FT5x06 initialized successfully");
    return ESP_OK;
}

/*
 * Get current touch point
 * Returns coordinates mapped to landscape (320x240) display
 */
bool touch_driver_get_point(touch_point_t *pt)
{
    if (!touch_initialized || !pt) return false;

    uint8_t data[6];

    /* Read number of touches + first touch coordinates (regs 0x02-0x07) */
    if (ft5x06_read_reg(FT5X06_REG_NUM_TOUCHES, data, 6) != ESP_OK) {
        return false;
    }

    uint8_t num_touches = data[0] & 0x0F;
    if (num_touches == 0) {
        return false;
    }

    /* Parse touch 1 coordinates (12-bit format) */
    uint16_t raw_x = ((uint16_t)(data[1] & 0x0F) << 8) | data[2];
    uint16_t raw_y = ((uint16_t)(data[3] & 0x0F) << 8) | data[4];

    /* Clamp to native panel bounds */
    if (raw_x >= FT5X06_NATIVE_X) raw_x = FT5X06_NATIVE_X - 1;
    if (raw_y >= FT5X06_NATIVE_Y) raw_y = FT5X06_NATIVE_Y - 1;

    /* Map portrait (240x320) to landscape (320x240):
     * landscape_x = native_y
     * landscape_y = native_width - native_x  (mirror Y to match display)
     */
    pt->x = raw_y;
    pt->y = FT5X06_NATIVE_X - 1 - raw_x;

    last_point = *pt;
    return true;
}
