/*
 * image_decoder.c - Image decoder with JPG/BMP support and auto-scaling
 * For ESP32-S3 with OPI PSRAM
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_decode.h"
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

    // Check BMP signature: 'BM'
    if (header[0] == 'B' && header[1] == 'M') {
        return IMG_FMT_BMP;
    }

    // Check JPEG signature: FF D8 FF
    if (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
        return IMG_FMT_JPEG;
    }

    return IMG_FMT_UNKNOWN;
}

// Decode BMP file
static bool decode_bmp(const char *filepath, decoded_image_t *out_image)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open BMP: %s", filepath);
        return false;
    }

    bmp_file_header_t file_hdr;
    bmp_info_header_t info_hdr;

    if (fread(&file_hdr, sizeof(file_hdr), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (file_hdr.bfType != 0x4D42) {
        ESP_LOGE(TAG, "Not a valid BMP file");
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

    ESP_LOGI(TAG, "BMP: %ldx%ld", (long)w, (long)h);

    // Allocate pixel buffer
    size_t pixel_count = (size_t)w * h;
    uint16_t *buf = heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(pixel_count * sizeof(uint16_t));
        if (!buf) {
            ESP_LOGE(TAG, "OOM for BMP pixels");
            fclose(f);
            return false;
        }
    }

    // Seek to pixel data
    fseek(f, file_hdr.bfOffBits, SEEK_SET);

    // BMP rows are aligned to 4 bytes
    int row_padding = (4 - ((w * 3) % 4)) % 4;

    for (int32_t y = 0; y < h; y++) {
        // BMP is bottom-up
        int32_t row = (info_hdr.biHeight > 0) ? (h - 1 - y) : y;

        for (int32_t x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (fread(bgr, 1, 3, f) != 3) {
                free(buf);
                fclose(f);
                return false;
            }
            // Convert BGR to RGB565
            uint8_t r = bgr[2] >> 3;
            uint8_t g = bgr[1] >> 2;
            uint8_t b = bgr[0] >> 3;
            buf[row * w + x] = (r << 11) | (g << 5) | b;
        }
        fseek(f, row_padding, SEEK_CUR);
    }

    fclose(f);

    out_image->pixels = buf;
    out_image->width = w;
    out_image->height = h;
    out_image->format = IMG_FMT_BMP;
    return true;
}

// Decode JPEG file using esp_jpeg
static bool decode_jpeg(const char *filepath, decoded_image_t *out_image)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open JPEG: %s", filepath);
        return false;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return false;
    }

    // Read entire file
    uint8_t *jpeg_data = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_data) {
        jpeg_data = malloc(file_size);
        if (!jpeg_data) {
            ESP_LOGE(TAG, "OOM for JPEG data");
            fclose(f);
            return false;
        }
    }

    if (fread(jpeg_data, 1, file_size, f) != (size_t)file_size) {
        free(jpeg_data);
        fclose(f);
        return false;
    }
    fclose(f);

    // Decode JPEG using esp_jpeg API
    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_dec_config_t config = {
        .output_type = JPEG_RAW_TYPE_RGB565_LE,
        .rotate = JPEG_ROTATE_0,
    };

    jpeg_dec = jpeg_dec_open(&config);
    if (!jpeg_dec) {
        ESP_LOGE(TAG, "jpeg_dec_open failed");
        free(jpeg_data);
        return false;
    }

    jpeg_dec_io_t jpeg_io = {
        .inbuf = jpeg_data,
        .inbuf_len = file_size,
        .inbuf_remain = file_size,
    };

    jpeg_dec_header_info_t out_info = {0};
    if (jpeg_dec_header(jpeg_dec, &jpeg_io, &out_info) != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_dec_header failed");
        jpeg_dec_close(jpeg_dec);
        free(jpeg_data);
        return false;
    }

    int width = out_info.width;
    int height = out_info.height;
    ESP_LOGI(TAG, "JPEG: %dx%d", width, height);

    // Allocate output buffer
    size_t out_size = width * height * 2; // RGB565 = 2 bytes per pixel
    uint16_t *out_buf = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_buf) {
        ESP_LOGE(TAG, "OOM for JPEG output");
        jpeg_dec_close(jpeg_dec);
        free(jpeg_data);
        return false;
    }

    jpeg_dec_process(jpeg_dec, &jpeg_io, &out_info, (uint8_t *)out_buf);
    jpeg_dec_close(jpeg_dec);
    free(jpeg_data);

    out_image->pixels = out_buf;
    out_image->width = width;
    out_image->height = height;
    out_image->format = IMG_FMT_JPEG;
    return true;
}

// Simple nearest-neighbor scaling
static bool scale_image(decoded_image_t *src, uint16_t target_w, uint16_t target_h, decoded_image_t *dst)
{
    if (src->width == target_w && src->height == target_h) {
        // No scaling needed, just copy
        *dst = *src;
        return true;
    }

    ESP_LOGI(TAG, "Scaling %dx%d -> %dx%d", src->width, src->height, target_w, target_h);

    size_t out_size = (size_t)target_w * target_h * sizeof(uint16_t);
    uint16_t *out_buf = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_buf) {
        out_buf = malloc(out_size);
        if (!out_buf) {
            ESP_LOGE(TAG, "OOM for scaled image");
            return false;
        }
    }

    // Nearest-neighbor scaling
    for (int y = 0; y < target_h; y++) {
        int src_y = y * src->height / target_h;
        for (int x = 0; x < target_w; x++) {
            int src_x = x * src->width / target_w;
            out_buf[y * target_w + x] = src->pixels[src_y * src->width + src_x];
        }
    }

    // Free source pixels if they were allocated
    if (src->pixels) {
        free(src->pixels);
        src->pixels = NULL;
    }

    dst->pixels = out_buf;
    dst->width = target_w;
    dst->height = target_h;
    dst->format = src->format;
    return true;
}

bool decode_image_file(const char *filepath, uint16_t target_width, uint16_t target_height, decoded_image_t *out_image)
{
    image_format_t fmt = detect_image_format(filepath);
    if (fmt == IMG_FMT_UNKNOWN) {
        ESP_LOGE(TAG, "Unknown image format: %s", filepath);
        return false;
    }

    decoded_image_t raw_image = {0};
    bool ok = false;

    switch (fmt) {
        case IMG_FMT_BMP:
            ok = decode_bmp(filepath, &raw_image);
            break;
        case IMG_FMT_JPEG:
            ok = decode_jpeg(filepath, &raw_image);
            break;
        default:
            break;
    }

    if (!ok) {
        return false;
    }

    // Scale to target size
    if (!scale_image(&raw_image, target_width, target_height, out_image)) {
        if (raw_image.pixels) free(raw_image.pixels);
        return false;
    }

    return true;
}

void free_decoded_image(decoded_image_t *image)
{
    if (image && image->pixels) {
        free(image->pixels);
        image->pixels = NULL;
    }
}
