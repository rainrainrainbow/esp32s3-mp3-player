#ifndef TFT_DRIVER_H
#define TFT_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

void tft_init(void);
void tft_set_backlight(uint8_t brightness);
void tft_fill_screen(uint16_t color);
void tft_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void tft_show_image_file(const char *filepath);

// Backlight control with PWM-like levels (0-100)
void tft_set_brightness(uint8_t percent);

#endif // TFT_DRIVER_H
