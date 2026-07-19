/*
 * main.c - ESP32-S3 MP3 Player (minimal-player branch)
 *
 * Minimal playback: one feature at a time
 * Hardware: ESP32-S3 N16R8, OPI PSRAM, ES8311, 240x320 TFT
 *
 * Features:
 * - MP3 playback from SPI flash FATFS via ES8311
 * - TFT image display (BMP slideshow, synced with track)
 * - GPIO0 = Prev track, GPIO43 = Next track
 * - Display "STOP" on screen when idle
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "config.h"
#include "tft_driver.h"
#include "audio_player.h"
#include "fatfs_manager.h"
#include "image_decoder.h"

static const char *TAG = "MAIN";

static uint8_t current_track = 0;

/* Check if filename has an image extension */
static bool is_image_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".bmp") == 0) return true;
    return false;
}

/* 5x7 pixel font bitmap for basic characters */
static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x00,0x08,0x14,0x22,0x41}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x41,0x22,0x14,0x08,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x01,0x01}, // F
    {0x3E,0x41,0x41,0x51,0x32}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x7F,0x20,0x18,0x20,0x7F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x03,0x04,0x78,0x04,0x03}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x00,0x7F,0x41,0x41}, // [
    {0x02,0x04,0x08,0x10,0x20}, // backslash
    {0x41,0x41,0x7F,0x00,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x08,0x14,0x54,0x54,0x3C}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x00,0x7F,0x10,0x28,0x44}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x20,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
};

/* Draw a character at (x, y) with given color */
static void draw_char(uint16_t x, uint16_t y, char c, uint16_t color)
{
    if (c < 32 || c > 126) c = 32;
    int idx = c - 32;
    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                // Draw a 2x2 pixel block for visibility
                tft_draw_pixel(x + col*2, y + row*2, color);
                tft_draw_pixel(x + col*2 + 1, y + row*2, color);
                tft_draw_pixel(x + col*2, y + row*2 + 1, color);
                tft_draw_pixel(x + col*2 + 1, y + row*2 + 1, color);
            }
        }
    }
}

/* Draw a string at (x, y) */
static void draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color)
{
    while (*str) {
        draw_char(x, y, *str, color);
        x += 12; // 5px char + 1px gap, doubled = 12
        str++;
    }
}

/* Display STOP screen */
static void display_stop(void)
{
    tft_fill_screen(0x0000);
    draw_string(72, 140, "STOP", 0xF800); // Red STOP at center
}

/* Show first image from images/<track>/ folder */
static void show_track_image(uint8_t track)
{
    char track_dir[64];
    snprintf(track_dir, sizeof(track_dir), "%s/%d", IMAGE_DIR, track);
    
    DIR *dir = opendir(track_dir);
    if (!dir) return;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (is_image_file(entry->d_name)) {
            char img_path[384];
            snprintf(img_path, sizeof(img_path), "%s/%s", track_dir, entry->d_name);
            closedir(dir);
            tft_show_image_file(img_path);
            return;
        }
    }
    closedir(dir);
}

/* Slideshow task - shows images from images/<track>/ while playing */
static void slideshow_task(void *param)
{
    uint8_t prev_track = 0;
    char image_paths[64][384];
    uint8_t img_count = 0;
    uint8_t img_index = 0;

    while (1) {
        if (audio_player_get_state() == PLAYER_STATE_PLAYING) {
            uint8_t track = audio_player_get_current_track();

            if (track != prev_track) {
                prev_track = track;
                img_count = 0;
                img_index = 0;

                char track_dir[64];
                snprintf(track_dir, sizeof(track_dir), "%s/%d", IMAGE_DIR, track);
                DIR *dir = opendir(track_dir);
                if (dir) {
                    struct dirent *entry;
                    while ((entry = readdir(dir)) != NULL && img_count < 64) {
                        if (is_image_file(entry->d_name)) {
                            strlcpy(image_paths[img_count], track_dir, sizeof(image_paths[0]));
                            strlcat(image_paths[img_count], "/", sizeof(image_paths[0]));
                            strlcat(image_paths[img_count], entry->d_name, sizeof(image_paths[0]));
                            img_count++;
                        }
                    }
                    closedir(dir);
                    ESP_LOGI(TAG, "Track %d: %d images", track, img_count);
                }
            }

            if (img_count > 0) {
                ESP_LOGI(TAG, "Image %d/%d", img_index + 1, img_count);
                tft_show_image_file(image_paths[img_index]);
                img_index = (img_index + 1) % img_count;
            }
        } else {
            // Not playing - show STOP
            display_stop();
            prev_track = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* Button task */
static void button_task(void *param)
{
    // Count tracks
    DIR *dir = opendir(MUSIC_DIR);
    uint8_t max_tracks = 0;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && strcasecmp(ext, ".mp3") == 0) max_tracks++;
        }
        closedir(dir);
    }
    ESP_LOGI(TAG, "Found %d MP3 tracks", max_tracks);
    if (max_tracks == 0) max_tracks = 255;

    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);
    gpio_set_direction(GPIO_NUM_43, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_43, GPIO_PULLUP_ONLY);

    uint8_t current = 1;
    while (1) {
        bool prev_pressed = (gpio_get_level(GPIO_NUM_0) == 0);
        bool next_pressed = (gpio_get_level(GPIO_NUM_43) == 0);

        if (prev_pressed) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(GPIO_NUM_0) == 0) {
                current = (current == 1) ? max_tracks : current - 1;
                ESP_LOGI(TAG, "Prev track %d", current);
                audio_player_play_track(current);
                while (gpio_get_level(GPIO_NUM_0) == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        if (next_pressed) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(GPIO_NUM_43) == 0) {
                current = (current >= max_tracks) ? 1 : current + 1;
                ESP_LOGI(TAG, "Next track %d", current);
                audio_player_play_track(current);
                while (gpio_get_level(GPIO_NUM_43) == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Minimal MP3 Player ===");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TFT
    tft_init();

    // Initialize audio
    audio_player_init();

    // Mount FATFS
    if (!fatfs_mount_spiflash()) {
        ESP_LOGE(TAG, "FATFS mount failed");
        tft_fill_screen(0xF800);
        return;
    }

    // Show STOP screen
    display_stop();

    // Start slideshow task
    xTaskCreatePinnedToCore(slideshow_task, "slideshow", 4096, NULL, 1, NULL, 1);

    // Start button task
    xTaskCreatePinnedToCore(button_task, "buttons", 3072, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "System ready");
    ESP_LOGI(TAG, "GPIO0=Prev, GPIO43=Next");
}
