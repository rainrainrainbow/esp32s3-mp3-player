#include "i2c_slave.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>
#include "driver/i2c_slave.h"

static const char *TAG = "I2C_SLAVE";

// Register storage (non-volatile, accessed only from task context)
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

// I2C channel
static i2c_slave_dev_handle_t slave_handle = NULL;

// Track last register address for read operations
static uint8_t last_reg_addr = 0;

/**
 * @brief I2C slave RX callback (called in ISR context when master writes data)
 */
static bool i2c_slave_rx_callback(i2c_slave_dev_handle_t slave, const i2c_slave_rx_event_data_t *event, void *user_data)
{
    if (event->received_bytes >= 1) {
        last_reg_addr = event->received_data[0];
    }
    
    // Process write commands (register + value)
    if (event->received_bytes >= 2) {
        uint8_t reg = event->received_data[0];
        uint8_t val = event->received_data[1];
        
        switch (reg) {
            case REG_PLAY_TRACK:
                if (val >= 1 && val <= 255) {
                    pending_track = val;
                    if (play_callback) play_callback(val);
                }
                break;
                
            case REG_PLAY_STATUS:
                if (val == STATUS_STOPPED) {
                    stop_requested = true;
                    if (stop_callback) stop_callback();
                }
                break;
                
            case REG_VOLUME:
                if (val <= 100) {
                    reg_volume = val;
                    if (volume_callback) volume_callback(val);
                }
                break;
                
            case REG_BRIGHTNESS:
                if (val <= 100) {
                    reg_brightness = val;
                    if (brightness_callback) brightness_callback(val);
                }
                break;
        }
    }
    
    return false;
}

/**
 * @brief I2C slave TX callback (called in ISR context when master requests data)
 */
static bool i2c_slave_tx_callback(i2c_slave_dev_handle_t slave, const i2c_slave_tx_event_data_t *event, void *user_data)
{
    uint8_t dataToSend = 0;
    
    switch (last_reg_addr) {
        case REG_PLAY_STATUS:
            dataToSend = reg_play_status;
            break;
        case REG_CURRENT_TRACK:
            dataToSend = reg_current_track;
            break;
        case REG_VOLUME:
            dataToSend = reg_volume;
            break;
        case REG_BRIGHTNESS:
            dataToSend = reg_brightness;
            break;
        default:
            dataToSend = reg_play_status;
            break;
    }
    
    size_t copy_size = event->transmit_bytes_requested;
    if (copy_size > 1) copy_size = 1;
    
    memcpy(event->buffer, &dataToSend, copy_size);
    
    return false;
}

void i2c_slave_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C slave at address 0x%02X", I2C_SLAVE_ADDR);
    
    i2c_slave_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SLAVE_SDA,
        .scl_io_num = I2C_SLAVE_SCL,
        .clk_source = SOC_MOD_CLK_APB,
        .send_buf_depth = 128,
        .slave_addr = I2C_SLAVE_ADDR,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .intr_priority = 0,
    };
    
    esp_err_t ret = i2c_new_slave_dev(&config, &slave_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C slave dev create failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Register event callbacks
    i2c_slave_event_callbacks_t cbs = {
        .on_recv = i2c_slave_rx_callback,
        .on_req = i2c_slave_tx_callback,
    };
    
    ret = i2c_slave_register_event_callbacks(slave_handle, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C slave register callbacks failed: %s", esp_err_to_name(ret));
        return;
    }
    
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
