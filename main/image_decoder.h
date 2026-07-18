#ifndef IMAGE_DECODER_H
#define IMAGE_DECODER_H

#include <stdint.h>
#include <stddef.h>

// Minimum JPEG decoder for ESP32-S3
// Decodes JPEG to RGB565 for TFT display
bool jpeg_decode_to_rgb565(const uint8_t *jpeg_data, size_t len,
                           uint16_t *out_buf, size_t buf_size,
                           uint16_t *width, uint16_t *height);

// BMP decoder (simple 24-bit BMP)
bool bmp_decode_to_rgb565(const uint8_t *bmp_data, size_t len,
                          uint16_t *out_buf, size_t buf_size,
                          uint16_t *width, uint16_t *height);

#endif // IMAGE_DECODER_H