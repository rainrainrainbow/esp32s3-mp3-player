#include "i2c_slave.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>
#include "driver/i2c.h"

static const char *TAG = "I2C_SLAVE";

// Register storage
static volatile uint8_t reg_play_status = STATUS_STOPPED;
static volatile uint8_t reg_current_track = 0;
static volatile uint8_t reg_volume = 50;
static volatile uint8_t reg_brightness = 75;

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

// Read buffer for register addressing
static uint8_t last_reg_addr = 0;

/**
 * @brief I2C slave event handler
 */
static void i2c_slave_event_handler(void *arg)
{
    i2c_slave_evt_t *event = (i2c_slave_evt_t *)arg;
    
    switch (event->info.event) {
        case I2C_SLAVE_EVT_RXFIFO: {
            // Data received from master
            int data_len = event->info.data_len;
            uint8_t data[32];
            
            int read_len = i2c_slave_read_buffer(I2C_SLAVE_PORT, data, data_len, 10);
            if (read_len > 0) {
                // First byte is register address
                last_reg_addr = data[0];
                
                // Process write commands (register + value)
                if (read_len >= 2) {
                    uint8_t reg = data[0];
                    uint8_t val = data[1];
                    
                    switch (reg) {
                        case REG_PLAY_TRACK:
                            if (val >= 1 && val <= 255) {
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
                    }
                }
            }
            break;
        }
        
        case I2C_SLAVE_EVT_TXFIFO:
            // Master requesting data (read operation)
            // Send data based on last register address
            switch (last_reg_addr) {
                case REG_PLAY_STATUS:
                    i2c_slave_write_buffer(I2C_SLAVE_PORT, &reg_play_status, 1, 10);
                    break;
                case REG_CURRENT_TRACK:
                    i2c_slave_write_buffer(I2C_SLAVE_PORT, &reg_current_track, 1, 10);
                    break;
                case REG_VOLUME:
                    i2c_slave_write_buffer(I2C_SLAVE_PORT, &reg_volume, 1, 10);
                    break;
                case REG_BRIGHTNESS:
                    i2c_slave_write_buffer(I2C_SLAVE_PORT, &reg_brightness, 1, 10);
                    break;
                default:
                    i2c_slave_write_buffer(I2C_SLAVE_PORT, &reg_play_status, 1, 10);
                    break;
            }
            break;
            
        default:
            break;
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
        .clk_flags = 0,
    };
    
    esp_err_t ret = i2c_param_config(I2C_SLAVE_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = i2c_driver_install(I2C_SLAVE_PORT, conf.mode, 1024, 1024, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Register event handler
    i2c_isr_register(I2C_SLAVE_PORT, i2c_slave_event_handler, NULL, 0, NULL);
    
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
