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

// Pending car task command (master polls via I2C, cleared on read)
static volatile uint8_t pending_task_cmd = TASK_CMD_NONE;

// Callbacks
static i2c_play_track_cb_t play_callback = NULL;
static i2c_stop_cb_t stop_callback = NULL;
static i2c_volume_cb_t volume_callback = NULL;
static i2c_brightness_cb_t brightness_callback = NULL;

// I2C port
#define I2C_SLAVE_PORT I2C_NUM_0

// Track last register address for read operations
static uint8_t last_reg_addr = 0;

/**
 * @brief Pre-load the TX buffer with the value of the given register address
 *        so that the I2C master's requestFrom() reads the correct register.
 *        Called when master does a single-byte register-set (read request).
 */
static void preload_reg_value(uint8_t reg)
{
    uint8_t val = 0;

    switch (reg) {
        case REG_PLAY_STATUS:
            val = reg_play_status;
            break;
        case REG_CURRENT_TRACK:
            val = reg_current_track;
            break;
        case REG_VOLUME:
            val = reg_volume;
            break;
        case REG_BRIGHTNESS:
            val = reg_brightness;
            break;
        case REG_TASK_COMMAND:
            // Read-once semantics: return pending command then clear it
            val = pending_task_cmd;
            pending_task_cmd = TASK_CMD_NONE;
            break;
        default:
            val = 0;
            break;
    }

    i2c_slave_write_buffer(I2C_SLAVE_PORT, &val, 1, pdMS_TO_TICKS(10));
}

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

            if (len == 1) {
                // Master only wrote a register address => it intends to READ that register.
                // Pre-load the matching value into the TX buffer.
                preload_reg_value(data[0]);
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

                    case REG_TASK_COMMAND:
                        if (val == TASK_CMD_ACTION1 || val == TASK_CMD_ACTION2) {
                            ESP_LOGI(TAG, "I2C: Task command %d", val);
                            pending_task_cmd = val;
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

void i2c_slave_set_task_command(uint8_t cmd)
{
    if (cmd == TASK_CMD_ACTION1 || cmd == TASK_CMD_ACTION2) {
        pending_task_cmd = cmd;
        ESP_LOGI(TAG, "I2C: task command set to %d", cmd);
    }
}

uint8_t i2c_slave_get_pending_task(void)
{
    uint8_t cmd = pending_task_cmd;
    pending_task_cmd = TASK_CMD_NONE;
    return cmd;
}
