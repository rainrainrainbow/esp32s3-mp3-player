/*
 * image_decoder.c - Image decoder with JPG/BMP support and hardware scaling
 * For ESP32-S3 with OPI PSRAM
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
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

    if (header[0] == 'B' && header[1] == 'M') return IMG_FMT_BMP;
    if (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) return IMG_FMT_JPEG;
    return IMG_FMT_UNKNOWN;
}

// Decode BMP directly to target size using nearest-neighbor (fast, no float math)
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

    if (file_hdr.bfType != 0x4D42) {
        ESP_LOGE(TAG, "Not a valid BMP file");
        fclose(f);
        return false;
    }

    if (info_hdr.biBitCount != 24) {
        ESP_LOGE(TAG, "Only 24-bit BMP supported, got %d-bit", info_hdr.biBitCount);
        fclose(f);
        return false;
    }

    int32_t src_w = info_hdr.biWidth;
    int32_t src_h = info_hdr.biHeight > 0 ? info_hdr.biHeight : -info_hdr.biHeight;
    bool bottom_up = (info_hdr.biHeight > 0);

    ESP_LOGI(TAG, "BMP: %ldx%ld -> %dx%d", (long)src_w, (long)src_h, target_w, target_h);

    // Read entire pixel data into memory first (avoid per-pixel fseek)
    int row_padding = (4 - ((src_w * 3) % 4)) % 4;
    int row_stride = src_w * 3 + row_padding;
    size_t pixel_data_size = (size_t)row_stride * src_h;
    
    uint8_t *pixel_data = heap_caps_malloc(pixel_data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixel_data) {
        pixel_data = malloc(pixel_data_size);
        if (!pixel_data) {
            ESP_LOGE(TAG, "OOM for BMP pixel data (%u bytes)", (unsigned)pixel_data_size);
            fclose(f);
            return false;
        }
    }

    fseek(f, file_hdr.bfOffBits, SEEK_SET);
    if (fread(pixel_data, 1, pixel_data_size, f) != pixel_data_size) {
        ESP_LOGE(TAG, "Short read on BMP pixel data");
        free(pixel_data);
        fclose(f);
        return false;
    }
    fclose(f);

    // Allocate output buffer at target size
    size_t out_count = (size_t)target_w * target_h;
    uint16_t *buf = heap_caps_malloc(out_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(out_count * sizeof(uint16_t));
        if (!buf) {
            ESP_LOGE(TAG, "OOM for BMP output");
            free(pixel_data);
            return false;
        }
    }

    // Nearest-neighbor scaling directly from memory (fast!)
    for (int y = 0; y < target_h; y++) {
        int src_y = y * src_h / target_h;
        int bmp_row = bottom_up ? (src_h - 1 - src_y) : src_y;
        const uint8_t *row_ptr = pixel_data + bmp_row * row_stride;

        for (int x = 0; x < target_w; x++) {
            int src_x = x * src_w / target_w;
            const uint8_t *px = row_ptr + src_x * 3;
            // BGR -> RGB565
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

// Decode JPEG using esp_jpeg hardware decoder with built-in scaling
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

    // Get original image info
    esp_jpeg_image_cfg_t cfg = {
        .indata = jpeg_data,
        .indata_size = file_size,
        .outbuf = NULL,
        .outbuf_size = 0,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {0},
        .advanced = {
            .working_buffer = NULL,
            .working_buffer_size = 0
        }
    };

    esp_jpeg_image_output_t out_info = {0};
    if (esp_jpeg_get_image_info(&cfg, &out_info) != ESP_OK) {
        ESP_LOGE(TAG, "esp_jpeg_get_image_info failed");
        free(jpeg_data);
        return false;
    }

    int src_w = out_info.width;
    int src_h = out_info.height;
    ESP_LOGI(TAG, "JPEG: %dx%d -> %dx%d", src_w, src_h, target_w, target_h);

    // Choose optimal hardware scale factor
    // esp_jpeg supports: 1:1, 1:2, 1:4, 1:8
    float scale_ratio_w = (float)target_w / src_w;
    float scale_ratio_h = (float)target_h / src_h;
    float scale_ratio = (scale_ratio_w < scale_ratio_h) ? scale_ratio_w : scale_ratio_h;

    jpeg_scale_t scale = JPEG_IMAGE_SCALE_0;
    int div = 1;
    if (scale_ratio <= 0.15f) {
        scale = JPEG_IMAGE_SCALE_1_8; div = 8;
    } else if (scale_ratio <= 0.3f) {
        scale = JPEG_IMAGE_SCALE_1_4; div = 4;
    } else if (scale_ratio <= 0.6f) {
        scale = JPEG_IMAGE_SCALE_1_2; div = 2;
    }

    int hw_w = src_w / div;
    int hw_h = src_h / div;

    ESP_LOGI(TAG, "HW scale 1:%d -> %dx%d", div, hw_w, hw_h);

    // Allocate output buffer at final target size
    // If HW scale gives exact size, decode directly to output
    // Otherwise decode to HW-scaled buffer then do fast nearest-neighbor
    size_t out_size = (size_t)target_w * target_h * 2;
    uint16_t *out_buf = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_buf) {
        ESP_LOGE(TAG, "OOM for JPEG output");
        free(jpeg_data);
        return false;
    }

    if (hw_w == target_w && hw_h == target_h) {
        // Perfect match - decode directly to output
        cfg.outbuf = (uint8_t *)out_buf;
        cfg.outbuf_size = out_size;
        cfg.out_scale = scale;

        if (esp_jpeg_decode(&cfg, &out_info) != ESP_OK) {
            ESP_LOGE(TAG, "esp_jpeg_decode failed");
            free(out_buf);
            free(jpeg_data);
            return false;
        }
        free(jpeg_data);

        out_image->pixels = out_buf;
        out_image->width = target_w;
        out_image->height = target_h;
        out_image->format = IMG_FMT_JPEG;
        return true;
    }

    // Decode to intermediate HW-scaled buffer
    size_t hw_size = (size_t)hw_w * hw_h * 2;
    uint16_t *hw_buf = heap_caps_malloc(hw_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hw_buf) {
        ESP_LOGE(TAG, "OOM for JPEG HW buffer");
        free(out_buf);
        free(jpeg_data);
        return false;
    }

    cfg.outbuf = (uint8_t *)hw_buf;
    cfg.outbuf_size = hw_size;
    cfg.out_scale = scale;

    if (esp_jpeg_decode(&cfg, &out_info) != ESP_OK) {
        ESP_LOGE(TAG, "esp_jpeg_decode failed");
        free(hw_buf);
        free(out_buf);
        free(jpeg_data);
        return false;
    }
    free(jpeg_data);

    // Fast nearest-neighbor from HW-scaled to target (integer math only)
    ESP_LOGI(TAG, "SW scale %dx%d -> %dx%d", hw_w, hw_h, target_w, target_h);
    for (int y = 0; y < target_h; y++) {
        int src_y = y * hw_h / target_h;
        for (int x = 0; x < target_w; x++) {
            int src_x = x * hw_w / target_w;
            out_buf[y * target_w + x] = hw_buf[src_y * hw_w + src_x];
        }
    }

    free(hw_buf);

    out_image->pixels = out_buf;
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
