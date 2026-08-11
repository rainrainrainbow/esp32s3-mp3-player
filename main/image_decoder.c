/*
 * image_decoder.c - Image decoder with JPG/BMP support and software scaling
 * For ESP32-S3 with OPI PSRAM (no hardware JPEG decoder)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "image_decoder.h"

static const char *TAG = "IMG_DEC";

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} bmp_file_header_t;

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

image_format_t detect_image_format(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return IMG_FMT_UNKNOWN;

    uint8_t header[4];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return IMG_FMT_UNKNOWN;
    }
    fclose(f);

    if (header[0] == 'B' && header[1] == 'M') return IMG_FMT_BMP;
    if (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) return IMG_FMT_JPEG;
    return IMG_FMT_UNKNOWN;
}

// Decode BMP directly to target size using nearest-neighbor
static bool decode_bmp(const char *filepath, uint16_t target_w, uint16_t target_h, decoded_image_t *out_image)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open BMP: %s", filepath);
        return false;
    }

    bmp_file_header_t file_hdr;
    bmp_info_header_t info_hdr;

    if (fread(&file_hdr, sizeof(file_hdr), 1, f) != 1 ||
        fread(&info_hdr, sizeof(info_hdr), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (file_hdr.bfType != 0x4D42 || info_hdr.biBitCount != 24) {
        ESP_LOGE(TAG, "Invalid BMP (type=0x%04X, bits=%d)", file_hdr.bfType, info_hdr.biBitCount);
        fclose(f);
        return false;
    }

    int32_t src_w = info_hdr.biWidth;
    int32_t src_h = info_hdr.biHeight > 0 ? info_hdr.biHeight : -info_hdr.biHeight;
    bool bottom_up = (info_hdr.biHeight > 0);

    ESP_LOGI(TAG, "BMP: %ldx%ld -> %dx%d", (long)src_w, (long)src_h, target_w, target_h);

    // Read entire pixel data into memory
    int row_padding = (4 - ((src_w * 3) % 4)) % 4;
    int row_stride = src_w * 3 + row_padding;
    size_t pixel_data_size = (size_t)row_stride * src_h;

    uint8_t *pixel_data = heap_caps_malloc(pixel_data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixel_data) pixel_data = malloc(pixel_data_size);
    if (!pixel_data) {
        ESP_LOGE(TAG, "OOM for BMP pixel data");
        fclose(f);
        return false;
    }

    fseek(f, file_hdr.bfOffBits, SEEK_SET);
    if (fread(pixel_data, 1, pixel_data_size, f) != pixel_data_size) {
        ESP_LOGE(TAG, "Short read on BMP pixel data");
        free(pixel_data);
        fclose(f);
        return false;
    }
    fclose(f);

    // Allocate output buffer
    size_t out_count = (size_t)target_w * target_h;
    uint16_t *buf = heap_caps_malloc(out_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(out_count * sizeof(uint16_t));
    if (!buf) {
        free(pixel_data);
        return false;
    }

    // Nearest-neighbor scaling directly from memory
    for (int y = 0; y < target_h; y++) {
        int src_y = y * src_h / target_h;
        int bmp_row = bottom_up ? (src_h - 1 - src_y) : src_y;
        const uint8_t *row_ptr = pixel_data + bmp_row * row_stride;

        for (int x = 0; x < target_w; x++) {
            int src_x = x * src_w / target_w;
            const uint8_t *px = row_ptr + src_x * 3;
            uint8_t r = px[2] >> 3;
            uint8_t g = px[1] >> 2;
            uint8_t b = px[0] >> 3;
            buf[y * target_w + x] = (r << 11) | (g << 5) | b;
        }
    }

    free(pixel_data);

    out_image->pixels = buf;
    out_image->width = target_w;
    out_image->height = target_h;
    out_image->format = IMG_FMT_BMP;
    return true;
}

// Parse JPEG SOF (Start of Frame) marker to get image dimensions
// This avoids needing to call esp_jpeg_decode twice
static bool jpeg_parse_dimensions(const uint8_t *data, size_t size, int *width, int *height)
{
    size_t offset = 2; // Skip SOI marker (0xFFD8)
    while (offset + 1 < size) {
        if (data[offset] != 0xFF) break;
        uint8_t marker = data[offset + 1];
        if (marker == 0xD8) { offset++; continue; } // SOI
        if (marker == 0xD9) break; // EOI
        if (marker == 0x00) { offset++; continue; } // Stuff byte

        if (offset + 3 >= size) break;
        uint16_t seg_len = ((uint16_t)data[offset + 2] << 8) | data[offset + 3];

        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            // SOF0, SOF1, SOF2 - contains dimensions
            if (offset + 9 >= size) break;
            *height = ((int)data[offset + 5] << 8) | data[offset + 6];
            *width  = ((int)data[offset + 7] << 8) | data[offset + 8];
            return true;
        }

        // Skip to next marker
        if (seg_len < 2) break;
        offset += 2 + seg_len;
    }
    return false;
}

// Decode JPEG using esp_jpeg v1.3.1 (software decoder), then nearest-neighbor scale to target
static bool decode_jpeg(const char *filepath, uint16_t target_w, uint16_t target_h, decoded_image_t *out_image)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open JPEG: %s", filepath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return false;
    }

    uint8_t *jpeg_data = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_data) jpeg_data = malloc(file_size);
    if (!jpeg_data) {
        fclose(f);
        return false;
    }

    if (fread(jpeg_data, 1, file_size, f) != (size_t)file_size) {
        free(jpeg_data);
        fclose(f);
        return false;
    }
    fclose(f);

    // First: parse JPEG SOF to get original dimensions
    int src_w = 0, src_h = 0;
    if (!jpeg_parse_dimensions(jpeg_data, file_size, &src_w, &src_h)) {
        ESP_LOGE(TAG, "Failed to parse JPEG dimensions");
        free(jpeg_data);
        return false;
    }
    ESP_LOGI(TAG, "JPEG: %dx%d -> %dx%d", src_w, src_h, target_w, target_h);

    // Allocate output buffer for full-size decode (RGB565 = 2 bytes/pixel)
    size_t full_size = (size_t)src_w * src_h * 2;
    uint16_t *full_buf = heap_caps_malloc(full_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!full_buf) full_buf = malloc(full_size);
    if (!full_buf) {
        free(jpeg_data);
        return false;
    }

    // Single call to esp_jpeg_decode with properly sized output buffer
    esp_jpeg_image_cfg_t cfg = {
        .indata = jpeg_data,
        .indata_size = (uint32_t)file_size,
        .outbuf = (uint8_t *)full_buf,
        .outbuf_size = (uint32_t)full_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {0},
        .advanced = {
            .working_buffer = NULL,
            .working_buffer_size = 0
        }
    };

    esp_jpeg_image_output_t out_info = {0};
    esp_err_t ret = esp_jpeg_decode(&cfg, &out_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_jpeg_decode failed: %s", esp_err_to_name(ret));
        free(full_buf);
        free(jpeg_data);
        return false;
    }

    free(jpeg_data);

    // If same size, just return
    if (src_w == target_w && src_h == target_h) {
        out_image->pixels = full_buf;
        out_image->width = target_w;
        out_image->height = target_h;
        out_image->format = IMG_FMT_JPEG;
        return true;
    }

    // Allocate target buffer
    size_t target_size = (size_t)target_w * target_h * sizeof(uint16_t);
    uint16_t *target_buf = heap_caps_malloc(target_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!target_buf) target_buf = malloc(target_size);
    if (!target_buf) {
        free(full_buf);
        return false;
    }

    // Nearest-neighbor scale from decoded to target
    for (int y = 0; y < target_h; y++) {
        int src_y = y * src_h / target_h;
        for (int x = 0; x < target_w; x++) {
            int src_x = x * src_w / target_w;
            target_buf[y * target_w + x] = full_buf[src_y * src_w + src_x];
        }
    }

    free(full_buf);

    out_image->pixels = target_buf;
    out_image->width = target_w;
    out_image->height = target_h;
    out_image->format = IMG_FMT_JPEG;
    return true;
}

bool decode_image_file(const char *filepath, uint16_t target_width, uint16_t target_height, decoded_image_t *out_image)
{
    image_format_t fmt = detect_image_format(filepath);
    if (fmt == IMG_FMT_UNKNOWN) {
        ESP_LOGE(TAG, "Unknown image format: %s", filepath);
        return false;
    }

    switch (fmt) {
        case IMG_FMT_BMP:
            return decode_bmp(filepath, target_width, target_height, out_image);
        case IMG_FMT_JPEG:
            return decode_jpeg(filepath, target_width, target_height, out_image);
        default:
            return false;
    }
}

void free_decoded_image(decoded_image_t *image)
{
    if (image && image->pixels) {
        free(image->pixels);
        image->pixels = NULL;
    }
}
