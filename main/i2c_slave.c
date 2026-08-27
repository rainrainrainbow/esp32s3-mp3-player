#include "i2c_slave.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "I2C_SLAVE";

// Register storage
static uint8_t reg_play_status = STATUS_STOPPED;
static uint8_t reg_current_track = 0;
static uint8_t reg_volume = 50;
static uint8_t reg_brightness = 75;

// Pending commands
static volatile uint8_t pending_track = 0;
static volatile bool stop_requested = false;

// Callbacks
static i2c_play_track_cb_t play_callback = NULL;
static i2c_stop_cb_t stop_callback = NULL;
static i2c_volume_cb_t volume_callback = NULL;
static i2c_brightness_cb_t brightness_callback = NULL;

// I2C port
#define I2C_SLAVE_PORT I2C_NUM_0

// Track last register address for read operations
static uint8_t last_reg_addr = 0;

// Command register (S3→Uno)
static uint8_t g_cmd_to_uno = 0;

/**
 * @brief I2C slave polling task
 */
static void i2c_slave_task(void *arg)
{
    uint8_t data[32];
    
    while (1) {
        // Try to read data from I2C slave buffer
        int len = i2c_slave_read_buffer(I2C_SLAVE_PORT, data, sizeof(data), pdMS_TO_TICKS(50));
        
        if (len > 0) {
            // First byte is register address
            last_reg_addr = data[0];
            
            // Handle command register read request (len == 1 means master wants to read)
            if (len == 1 && data[0] == REG_COMMAND) {
                // Master wants to read command register
                // Prepare response in TX buffer
                uint8_t cmd_val = g_cmd_to_uno;  // Copy to local variable
                i2c_slave_write_buffer(I2C_SLAVE_PORT, &cmd_val, 1, pdMS_TO_TICKS(10));
                // Clear after sending (edge-triggered)
                g_cmd_to_uno = 0;
                continue;
            }
            
            // Process write commands (register + value)
            if (len >= 2) {
                uint8_t reg = data[0];
                uint8_t val = data[1];
                
                switch (reg) {
                    case REG_PLAY_TRACK:
                        if (val >= 1) {
                            ESP_LOGI(TAG, "I2C: Play track %d", val);
                            pending_track = val;
                            if (play_callback) play_callback(val);
                        }
                        break;
                        
                    case REG_PLAY_STATUS:
                        if (val == STATUS_STOPPED) {
                            ESP_LOGI(TAG, "I2C: Stop requested");
                            stop_requested = true;
                            if (stop_callback) stop_callback();
                        }
                        break;
                        
                    case REG_VOLUME:
                        if (val <= 100) {
                            ESP_LOGI(TAG, "I2C: Volume = %d", val);
                            reg_volume = val;
                            if (volume_callback) volume_callback(val);
                        }
                        break;
                        
                    case REG_BRIGHTNESS:
                        if (val <= 100) {
                            ESP_LOGI(TAG, "I2C: Brightness = %d", val);
                            reg_brightness = val;
                            if (brightness_callback) brightness_callback(val);
                        }
                        break;
                        
                    case REG_COMMAND:
                        // Uno writes command to S3 (e.g., from physical button)
                        if (val == CMD_TASK1 || val == CMD_TASK2 || val == CMD_STOP_TASK) {
                            ESP_LOGI(TAG, "I2C: Command 0x%02X received", val);
                            g_cmd_to_uno = val;
                        }
                        break;
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void i2c_slave_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C slave at address 0x%02X", I2C_SLAVE_ADDR);
    
    i2c_config_t conf = {
        .mode = I2C_MODE_SLAVE,
        .sda_io_num = I2C_SLAVE_SDA,
        .scl_io_num = I2C_SLAVE_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .slave.addr_10bit_en = 0,
        .slave.slave_addr = I2C_SLAVE_ADDR,
    };
    
    esp_err_t ret = i2c_param_config(I2C_SLAVE_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // i2c_driver_install(port, mode, rx_buf_len, tx_buf_len, intr_alloc_flags)
    ret = i2c_driver_install(I2C_SLAVE_PORT, I2C_MODE_SLAVE, 1024, 1024, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Pre-load initial register values into TX buffer
    i2c_slave_write_buffer(I2C_SLAVE_PORT, (const uint8_t *)&reg_play_status, 1, pdMS_TO_TICKS(10));
    
    // Create polling task
    xTaskCreate(i2c_slave_task, "i2c_slave", 2048, NULL, 10, NULL);
    
    ESP_LOGI(TAG, "I2C slave initialized: SDA=%d, SCL=%d", I2C_SLAVE_SDA, I2C_SLAVE_SCL);
}

void i2c_slave_set_callbacks(
    i2c_play_track_cb_t play_cb,
    i2c_stop_cb_t stop_cb,
    i2c_volume_cb_t volume_cb,
    i2c_brightness_cb_t brightness_cb)
{
    play_callback = play_cb;
    stop_callback = stop_cb;
    volume_callback = volume_cb;
    brightness_callback = brightness_cb;
}

void i2c_slave_set_status(uint8_t status)
{
    reg_play_status = status;
    // Pre-load into TX buffer for master read
    i2c_slave_write_buffer(I2C_SLAVE_PORT, (const uint8_t *)&reg_play_status, 1, pdMS_TO_TICKS(10));
}

void i2c_slave_set_current_track(uint8_t track)
{
    reg_current_track = track;
}

void i2c_slave_set_volume(uint8_t volume)
{
    reg_volume = volume;
}

void i2c_slave_set_brightness(uint8_t brightness)
{
    reg_brightness = brightness;
}

uint8_t i2c_slave_get_pending_track(void)
{
    return pending_track;
}

bool i2c_slave_get_stop_requested(void)
{
    return stop_requested;
}

void i2c_slave_clear_stop_request(void)
{
    stop_requested = false;
}

void i2c_slave_set_command(uint8_t cmd)
{
    if (cmd == CMD_TASK1 || cmd == CMD_TASK2 || cmd == CMD_STOP_TASK) {
        g_cmd_to_uno = cmd;
        ESP_LOGI(TAG, "Command set: 0x%02X", cmd);
    }
}
