#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdint.h>
#include <stdbool.h>

// I2C Slave Address
#define I2C_SLAVE_ADDR          0x52

// Register Map
#define REG_PLAY_TRACK          0x01  // Write: play track 1-255
#define REG_PLAY_STATUS         0x02  // Read: current status
#define REG_CURRENT_TRACK       0x03  // Read: current track number
#define REG_VOLUME              0x04  // Read/Write: volume 0-100
#define REG_BRIGHTNESS          0x05  // Read/Write: brightness 0-100

// Play Status
#define STATUS_STOPPED          0
#define STATUS_PLAYING          1
#define STATUS_PAUSED           2
#define STATUS_CHANGING         3
#define STATUS_ERROR            4

// Callback types
typedef void (*i2c_play_track_cb_t)(uint8_t track);
typedef void (*i2c_stop_cb_t)(void);
typedef void (*i2c_volume_cb_t)(uint8_t volume);
typedef void (*i2c_brightness_cb_t)(uint8_t brightness);

/**
 * @brief Initialize I2C slave
 */
void i2c_slave_init(void);

/**
 * @brief Set playback callbacks
 */
void i2c_slave_set_callbacks(
    i2c_play_track_cb_t play_cb,
    i2c_stop_cb_t stop_cb,
    i2c_volume_cb_t volume_cb,
    i2c_brightness_cb_t brightness_cb
);

/**
 * @brief Update current playback status
 */
void i2c_slave_set_status(uint8_t status);

/**
 * @brief Update current track number
 */
void i2c_slave_set_current_track(uint8_t track);

/**
 * @brief Update volume register (for I2C read)
 */
void i2c_slave_set_volume(uint8_t volume);

/**
 * @brief Update brightness register (for I2C read)
 */
void i2c_slave_set_brightness(uint8_t brightness);

/**
 * @brief Get pending track (returns 0 if none)
 */
uint8_t i2c_slave_get_pending_track(void);

/**
 * @brief Check if stop was requested
 */
bool i2c_slave_get_stop_requested(void);

/**
 * @brief Clear stop request flag
 */
void i2c_slave_clear_stop_request(void);

#endif // I2C_SLAVE_H
