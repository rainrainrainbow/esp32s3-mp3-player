/*
 * i2c_slave.c - I2C slave device at address 0x52
 * Registers:
 *   0x01: Play track (write) - select track 1-255
 *   0x02: Play status (read) - 0=stopped, 1=playing, 2=paused
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

// I2C slave callback context
typedef struct {
    uint8_t reg_addr;       // Last register address written by master
    bool reg_addr_set;      // Whether reg_addr has been set
} i2c_slave_ctx_t;

static i2c_slave_ctx_t slave_ctx;

static esp_err_t i2c_slave_rx_cb(i2c_slave_rx_done_event_data_t *data)
{
    // Process received data
    uint8_t *buf = data->buffer;
    size_t len = data->num_bytes;

    if (len == 1) {
        // Single byte: could be register address or data
        // If previous operation set a register address, this is data for that register
        if (slave_ctx.reg_addr_set) {
            regs[slave_ctx.reg_addr & 0xFF] = buf[0];
            ESP_LOGI(TAG, "Write reg 0x%02X = 0x%02X", slave_ctx.reg_addr, buf[0]);

            // Handle special registers
            if (slave_ctx.reg_addr == REG_PLAY_TRACK) {
                if (buf[0] > 0) {
                    ESP_LOGI(TAG, "Play track: %d", buf[0]);
                    audio_player_play_track(buf[0]);
                }
            }
            slave_ctx.reg_addr_set = false;
        } else {
            // First byte is register address
            slave_ctx.reg_addr = buf[0];
            slave_ctx.reg_addr_set = true;
        }
    } else if (len >= 2) {
        // Multi-byte: first byte is register address, rest are data
        slave_ctx.reg_addr = buf[0];
        for (size_t i = 1; i < len; i++) {
            regs[(slave_ctx.reg_addr + i - 1) & 0xFF] = buf[i];
        }
        // Handle play track
        if (buf[0] == REG_PLAY_TRACK && len >= 2) {
            if (buf[1] > 0) {
                ESP_LOGI(TAG, "Play track: %d", buf[1]);
                audio_player_play_track(buf[1]);
            }
        }
        slave_ctx.reg_addr_set = false;
    }

    return ESP_OK;
}

static esp_err_t i2c_slave_tx_cb(i2c_slave_tx_done_event_data_t *data)
{
    return ESP_OK;
}

void i2c_slave_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C slave at addr 0x%02X", I2C_SLAVE_ADDR);

    // Initialize slave context
    memset(&slave_ctx, 0, sizeof(slave_ctx));

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

    // Install I2C slave driver with receive buffer and transmit buffer
    ESP_ERROR_CHECK(i2c_driver_install(I2C_SLAVE_PORT, I2C_MODE_SLAVE,
                                       256,    // RX buffer size
                                       256,    // TX buffer size
                                       0));

    // Set up callbacks using the low-level API
    // Register receive callback
    i2c_slave_event_callbacks_t cbs = {
        .on_recv_done = i2c_slave_rx_cb,
        .on_send_done = i2c_slave_tx_cb,
    };
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(I2C_SLAVE_PORT, &cbs, NULL));

    // Initialize default register values
    regs[REG_PLAY_STATUS] = PLAYER_STATE_STOPPED;
    regs[REG_PLAY_TRACK] = 0;

    ESP_LOGI(TAG, "I2C slave initialized");
}

uint8_t i2c_slave_read_reg(uint8_t reg)
{
    return regs[reg & 0xFF];
}

void i2c_slave_write_reg(uint8_t reg, uint8_t val)
{
    regs[reg & 0xFF] = val;
}