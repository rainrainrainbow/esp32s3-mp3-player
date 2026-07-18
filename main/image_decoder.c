/*
 * image_decoder.c - Image decoder (BMP only, zero external dependencies)
 * For ESP32-S3 with OPI PSRAM
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "heap_caps.h"
#include "image_decoder.h"

static const char *TAG = "IMG_DEC";

// BMP file header (14 bytes)
#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} bmp_file_header_t;

// BMP info header (40 bytes)
typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} bmp_info_header_t;
#pragma pack(pop)

bool decode_bmp_file(const char *filepath, uint16_t *width, uint16_t *height, uint16_t **pixels)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", filepath);
        return false;
    }

    bmp_file_header_t file_hdr;
    bmp_info_header_t info_hdr;

    if (fread(&file_hdr, sizeof(file_hdr), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (file_hdr.bfType != 0x4D42) { // 'BM'
        ESP_LOGE(TAG, "Not a BMP file");
        fclose(f);
        return false;
    }

    if (fread(&info_hdr, sizeof(info_hdr), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (info_hdr.biBitCount != 24) {
        ESP_LOGE(TAG, "Only 24-bit BMP supported, got %d-bit", info_hdr.biBitCount);
        fclose(f);
        return false;
    }

    int32_t w = info_hdr.biWidth;
    int32_t h = info_hdr.biHeight > 0 ? info_hdr.biHeight : -info_hdr.biHeight;
    
    if (w > 320 || h > 240) {
        ESP_LOGW(TAG, "Image too large: %dx%d, will crop to display size", w, h);
    }

    // Allocate pixel buffer via PSRAM (external memory)
    size_t pixel_count = (size_t)w * h;
    uint16_t *buf = (uint16_t *)heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint16_t *)malloc(pixel_count * sizeof(uint16_t));
        if (!buf) {
            ESP_LOGE(TAG, "OOM for pixels");
            fclose(f);
            return false;
        }
    }

    // Seek to pixel data
    fseek(f, file_hdr.bfOffBits, SEEK_SET);

    // BMP rows are aligned to 4 bytes
    int row_padding = (4 - ((w * 3) % 4)) % 4;

    for (int32_t y = 0; y < h && y < 240; y++) {
        // BMP is bottom-up, so reverse row order if biHeight > 0
        int32_t row = (info_hdr.biHeight > 0) ? (h - 1 - y) : y;
        
        for (int32_t x = 0; x < w && x < 320; x++) {
            uint8_t bgr[3];
            if (fread(bgr, 1, 3, f) != 3) {
                goto cleanup;
            }
            // Convert BGR to RGB565
            uint8_t r = bgr[2] >> 3;  // 5 bits red
            uint8_t g = bgr[1] >> 2;  // 6 bits green
            uint8_t b = bgr[0] >> 3;  // 5 bits blue
            buf[row * w + x] = (r << 11) | (g << 5) | b;
        }
        // Skip padding bytes
        fseek(f, row_padding, SEEK_CUR);
    }

    fclose(f);
    *width = w;
    *height = h;
    *pixels = buf;
    return true;

cleanup:
    fclose(f);
    free(buf);
    return false;
}

void free_decoded_image(uint16_t *pixels)
{
    if (pixels) free(pixels);
}
