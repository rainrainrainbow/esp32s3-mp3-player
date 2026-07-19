#ifndef TFT_DRIVER_H
#define TFT_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

void tft_init(void);
void tft_set_backlight(uint8_t brightness);
void tft_fill_screen(uint16_t color);
void tft_draw_jpeg(const uint8_t *jpeg_data, size_t len, uint16_t x, uint16_t y);
void tft_draw_bmp(const uint8_t *bmp_data, size_t len);
void tft_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void tft_show_image_file(const char *filepath);

#endif // TFT_DRIVER_H