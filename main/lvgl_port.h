/*
 * lvgl_port.h - LVGL display and touch port for ESP32-S3
 */

#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize LVGL and the display/touch drivers
 */
void lvgl_port_init(void);

/**
 * @brief Get the LVGL tick timer value (in ms)
 */
uint32_t lvgl_port_tick_get(void);

/**
 * @brief Lock LVGL from other tasks
 */
bool lvgl_port_lock(int timeout_ms);

/**
 * @brief Unlock LVGL
 */
void lvgl_port_unlock(void);

#endif // LVGL_PORT_H
