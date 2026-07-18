/*
 * image_decoder.c - JPEG decoder using TJpgDec + BMP decoder
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "image_decoder.h"

// TJpgDec
#include "tjpgd.h"

static const char *TAG = "IMG_DEC";

/* TJpgDec input stream callback */
static size_t jpeg_input_cb(JDEC *jd, uint8_t *buf, size_t len)
{
    struct {
        const uint8_t *data;
        size_t size;
        size_t pos;
    } *ctx = (void*)jd->device;

    if (buf) {
        size_t available = ctx->size - ctx->pos;
        size_t to_read = (len < available) ? len : available;
        if (to_read > 0) {
            memcpy(buf, ctx->data + ctx->pos, to_read);
            ctx->pos += to_read;
        }
        return to_read;
    }
    size_t available = ctx->size - ctx->pos;
    size_t to_skip = (len < available) ? len : available;
    ctx->pos += to_skip;
    return to_skip;
}

/* TJpgDec output callback - fill RGB565 framebuffer */
static int jpeg_output_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    uint16_t *fb = (uint16_t *)jd->device;
    int fb_width = jd->width;

    uint8_t *src = (uint8_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int x = rect->left; x <= rect->right; x++) {
            uint8_t r = *src++;
            uint8_t g = *src++;
            uint8_t b = *src++;
            uint16_t pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            fb[y * fb_width + x] = pixel;
        }
    }
    return 1;
}

bool jpeg_decode_to_rgb565(const uint8_t *jpeg_data, size_t len,
                           uint16_t *out_buf, size_t buf_size,
                           uint16_t *width, uint16_t *height)
{
    struct {
        const uint8_t *data;
        size_t size;
        size_t pos;
    } stream_ctx = {
        .data = jpeg_data,
        .size = len,
        .pos = 0,
    };

    uint8_t *work = malloc(3500);
    if (!work) {
        ESP_LOGE(TAG, "OOM for TJpgDec work area");
        return false;
    }

    JDEC jd;
    JRESULT res = jd_prepare(&jd, jpeg_input_cb, work, 3500, &stream_ctx);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare failed: %d", res);
        free(work);
        return false;
    }

    *width = jd.width;
    *height = jd.height;
    ESP_LOGI(TAG, "JPEG: %dx%d", jd.width, jd.height);

    uint32_t needed = jd.width * jd.height * 2;
    if (needed > buf_size) {
        ESP_LOGE(TAG, "Buffer too small: need %d, have %d", needed, buf_size);
        free(work);
        return false;
    }

    jd.device = out_buf;
    res = jd_decomp(&jd, jpeg_output_cb, 0);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp failed: %d", res);
        free(work);
        return false;
    }

    free(work);
    ESP_LOGI(TAG, "JPEG decoded successfully");
    return true;
}

/* ====== BMP Decoder (24-bit RGB) ====== */
bool bmp_decode_to_rgb565(const uint8_t *bmp_data, size_t len,
                          uint16_t *out_buf, size_t buf_size,
                          uint16_t *width, uint16_t *height)
{
    if (len < 54) return false;
    if (bmp_data[0] != 'B' || bmp_data[1] != 'M') return false;

    uint32_t data_offset = *(uint32_t*)(bmp_data + 10);
    int w = *(int32_t*)(bmp_data + 18);
    int h = *(int32_t*)(bmp_data + 22);
    uint16_t bpp = *(uint16_t*)(bmp_data + 28);

    if (w <= 0 || h <= 0) return false;
    if (w > 320 || h > 480) return false;
    if ((uint32_t)(w * h) > buf_size / 2) return false;

    *width = w;
    *height = h;
    int abs_h = (h < 0) ? -h : h;

    int row_bytes = ((w * (bpp / 8) + 3) / 4) * 4;

    for (int y = abs_h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            int src_idx = data_offset + y * row_bytes + x * 3;
            if (src_idx + 2 >= (int)len) break;

            uint8_t b = bmp_data[src_idx];
            uint8_t g = bmp_data[src_idx + 1];
            uint8_t r = bmp_data[src_idx + 2];

            uint16_t pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            out_buf[y * w + x] = pixel;
        }
    }

    ESP_LOGI(TAG, "BMP decoded: %dx%d", w, abs_h);
    return true;
}