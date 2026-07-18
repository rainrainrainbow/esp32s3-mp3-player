/*
 * i2c_slave.c - I2C slave device at address 0x52 (IDF v4.4 compatible)
 * Registers:
 *   0x01: Play track (write) - select track 1-255
 *   0x02: Play status (read) - 0=stopped, 1=playing, 2=paused
 *
 * Uses polling-based i2c_slave_read_buffer() instead of callbacks
 * (IDF v4.4 does not have i2c_slave_register_event_callbacks)
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "config.h"
#include "i2c_slave.h"
#include "audio_player.h"

static const char *TAG = "I2C_SLAVE";

// Internal register storage
static uint8_t regs[256] = {0};

// I2C slave polling task handle
static TaskHandle_t i2c_slave_task_handle = NULL;

// Register address that master last wrote (for combined transactions)
static uint8_t last_reg_addr = 0;
static bool last_reg_addr_valid = false;

// Buffer for slave to send back to master
static uint8_t tx_buffer[256];

static void i2c_slave_polling_task(void *arg)
{
    uint8_t rx_buf[64];
    ESP_LOGI(TAG, "I2C slave polling task started");

    while (1) {
        // Blocking read from I2C master (master writing to us)
        size_t len = i2c_slave_read_buffer(I2C_SLAVE_PORT, rx_buf, sizeof(rx_buf), portMAX_DELAY);

        if (len > 0) {
            if (len == 1) {
                // Single byte received
                if (last_reg_addr_valid) {
                    // This is data for the previously set register
                    regs[last_reg_addr & 0xFF] = rx_buf[0];
                    ESP_LOGD(TAG, "Write reg 0x%02X = 0x%02X", last_reg_addr, rx_buf[0]);

                    if (last_reg_addr == REG_PLAY_TRACK && rx_buf[0] > 0) {
                        ESP_LOGI(TAG, "Play track: %d", rx_buf[0]);
                        audio_player_play_track(rx_buf[0]);
                    }
                    last_reg_addr_valid = false;
                } else {
                    // First byte is a register address (for future read or write)
                    last_reg_addr = rx_buf[0];
                    last_reg_addr_valid = true;
                    ESP_LOGD(TAG, "Set reg addr: 0x%02X", last_reg_addr);

                    // Pre-load TX buffer with register value for upcoming master read
                    tx_buffer[0] = regs[last_reg_addr & 0xFF];
                    i2c_slave_write_buffer(I2C_SLAVE_PORT, tx_buffer, 1, 100 / portTICK_PERIOD_MS);
                }
            } else if (len >= 2) {
                // Multi-byte: first byte is register address, rest are data
                last_reg_addr = rx_buf[0];
                for (size_t i = 1; i < len; i++) {
                    regs[(last_reg_addr + i - 1) & 0xFF] = rx_buf[i];
                }
                ESP_LOGD(TAG, "Write reg 0x%02X = 0x%02X (multi)", last_reg_addr, rx_buf[1]);

                if (rx_buf[0] == REG_PLAY_TRACK && rx_buf[1] > 0) {
                    ESP_LOGI(TAG, "Play track: %d", rx_buf[1]);
                    audio_player_play_track(rx_buf[1]);
                }
                last_reg_addr_valid = false;
            }
        }
    }
}

void i2c_slave_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C slave at addr 0x%02X", I2C_SLAVE_ADDR);

    // Initialize register with defaults
    memset(regs, 0, sizeof(regs));
    regs[REG_PLAY_STATUS] = PLAYER_STATE_STOPPED;

    // Configure I2C slave
    i2c_config_t conf = {
        .mode = I2C_MODE_SLAVE,
        .sda_io_num = I2C_SLAVE_SDA,
        .scl_io_num = I2C_SLAVE_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .slave.addr_10bit_en = 0,
        .slave.slave_addr = I2C_SLAVE_ADDR,
        .slave.maximum_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_SLAVE_PORT, &conf));

    // Install I2C slave driver
    ESP_ERROR_CHECK(i2c_driver_install(I2C_SLAVE_PORT, I2C_MODE_SLAVE,
                                       256,    // RX buffer
                                       256,    // TX buffer
                                       0));

    // Create polling task to handle I2C slave reads
    xTaskCreatePinnedToCore(i2c_slave_polling_task, "i2c_slave", 4096, NULL, 10, &i2c_slave_task_handle, 1);

    ESP_LOGI(TAG, "I2C slave initialized");
}

uint8_t i2c_slave_read_reg(uint8_t reg)
{
    return regs[reg & 0xFF];
}

void i2c_slave_write_reg(uint8_t reg, uint8_t val)
{
    regs[reg & 0xFF] = val;
    // Update TX buffer for pending master reads
    if (last_reg_addr_valid && last_reg_addr == (reg & 0xFF)) {
        tx_buffer[0] = val;
        i2c_slave_write_buffer(I2C_SLAVE_PORT, tx_buffer, 1, 100 / portTICK_PERIOD_MS);
    }
}
