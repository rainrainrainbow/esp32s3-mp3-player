/*
 * touch_driver.h - Touch screen driver interface
 * Supports XPT2046 (SPI resistive) and CST816S (I2C capacitive)
 */

#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Touch point structure */
typedef struct {
    uint16_t x;
    uint16_t y;
} touch_point_t;

/**
 * @brief Initialize the touch driver
 * @return ESP_OK on success
 */
esp_err_t touch_driver_init(void);

/**
 * @brief Get current touch point
 * @param pt Output touch point
 * @return true if touched, false if not
 */
bool touch_driver_get_point(touch_point_t *pt);

#endif // TOUCH_DRIVER_H
