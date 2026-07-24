/*
 * main.c - ESP32-S3 MP3 Player (minimal-player branch)
 *
 * Hardware: ESP32-S3 N16R8, OPI PSRAM, ES8311, 240x320 TFT
 *
 * Features:
 * - MP3 playback from SPI flash FATFS via ES8311
 * - TFT image display (BMP/JPG slideshow, synced with track)
 * - GPIO0 = Prev track, GPIO43 = Next track
 * - Settings menu with volume/brightness control
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "config.h"
#include "tft_driver.h"
#include "audio_player.h"
#include "fatfs_manager.h"
#include "usb_msc.h"
#include "image_decoder.h"

static const char *TAG = "MAIN";

// Settings stored in NVS
static uint8_t g_volume = 50;      // 0-100
static uint8_t g_brightness = 75;  // 0-100

// Settings menu state
typedef enum {
    MENU_NONE,
    MENU_MAIN,
    MENU_VOLUME,
    MENU_BRIGHTNESS
} menu_state_t;

static menu_state_t g_menu_state = MENU_NONE;

/* Check if filename has an image extension (BMP or JPG) */
static bool is_image_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".bmp") == 0) return true;
    if (strcasecmp(ext, ".jpg") == 0) return true;
    if (strcasecmp(ext, ".jpeg") == 0) return true;
    return false;
}

/* Load settings from NVS */
static void load_settings(void)
{
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "volume", &g_volume);
        nvs_get_u8(nvs, "brightness", &g_brightness);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Settings loaded: volume=%d, brightness=%d", g_volume, g_brightness);
    } else {
        ESP_LOGI(TAG, "No saved settings, using defaults");
    }
}

/* Save settings to NVS */
static void save_settings(void)
{
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "volume", g_volume);
        nvs_set_u8(nvs, "brightness", g_brightness);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Settings saved: volume=%d, brightness=%d", g_volume, g_brightness);
    }
}

/* Apply volume setting to audio */
static void apply_volume(void)
{
    audio_player_set_volume(g_volume);
}

/* Apply brightness setting to display */
static void apply_brightness(void)
{
    tft_set_brightness(g_brightness);
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
        x += 12;
        str++;
    }
}

/* Draw a progress bar */
static void draw_progress_bar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t percent, uint16_t color)
{
    // Draw border
    for (uint16_t i = 0; i < w; i++) {
        tft_draw_pixel(x + i, y, color);
        tft_draw_pixel(x + i, y + h - 1, color);
    }
    for (uint16_t i = 0; i < h; i++) {
        tft_draw_pixel(x, y + i, color);
        tft_draw_pixel(x + w - 1, y + i, color);
    }
    // Draw fill
    uint16_t fill_w = (w - 2) * percent / 100;
    for (uint16_t i = 0; i < fill_w; i++) {
        for (uint16_t j = 1; j < h - 1; j++) {
            tft_draw_pixel(x + 1 + i, y + j, color);
        }
    }
}

/* Display STOP screen */
static void display_stop(void)
{
    tft_fill_screen(0x0000);
    draw_string(72, 140, "STOP", 0xF800);
}

/* Display settings menu */
static void display_settings_menu(void)
{
    tft_fill_screen(0x0000);
    
    switch (g_menu_state) {
        case MENU_MAIN:
            draw_string(60, 20, "SETTINGS", 0xFFFF);
            draw_string(20, 60, "1. Volume", 0x07FF);
            draw_string(20, 100, "2. Brightness", 0x07FF);
            draw_string(20, 160, "Press GPIO0 to exit", 0xF800);
            break;
            
        case MENU_VOLUME:
            draw_string(60, 20, "VOLUME", 0xFFFF);
            draw_string(20, 60, "GPIO43: +10", 0x07FF);
            draw_string(20, 100, "GPIO0: -10", 0x07FF);
            draw_string(20, 160, "Value:", 0xFFFF);
            char vol_str[8];
            snprintf(vol_str, sizeof(vol_str), "%d%%", g_volume);
            draw_string(100, 160, vol_str, 0x07FF);
            draw_progress_bar(20, 200, 200, 20, g_volume, 0x07FF);
            draw_string(20, 260, "Long GPIO0: Back", 0xF800);
            break;
            
        case MENU_BRIGHTNESS:
            draw_string(40, 20, "BRIGHTNESS", 0xFFFF);
            draw_string(20, 60, "GPIO43: +10", 0x07FF);
            draw_string(20, 100, "GPIO0: -10", 0x07FF);
            draw_string(20, 160, "Value:", 0xFFFF);
            char bri_str[8];
            snprintf(bri_str, sizeof(bri_str), "%d%%", g_brightness);
            draw_string(100, 160, bri_str, 0x07FF);
            draw_progress_bar(20, 200, 200, 20, g_brightness, 0xFFFF);
            draw_string(20, 260, "Long GPIO0: Back", 0xF800);
            break;
            
        default:
            break;
    }
}

#define FILE_LIST_MAX_DEPTH 6

static void list_flash_tree(const char *path, int depth)
{
    if (depth > FILE_LIST_MAX_DEPTH) {
        ESP_LOGW(TAG, "%*s... depth limit at %s", depth * 2, "", path);
        return;
    }

    errno = 0;
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "opendir('%s') failed: errno=%d (%s)",
                 path, errno, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[384];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || n >= (int)sizeof(child)) {
            ESP_LOGW(TAG, "Path too long under %s: %s", path, entry->d_name);
            continue;
        }
        struct stat st;
        errno = 0;
        if (stat(child, &st) != 0) {
            ESP_LOGE(TAG, "stat('%s') failed: errno=%d (%s)",
                     child, errno, strerror(errno));
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            ESP_LOGI(TAG, "%*s[DIR ] %s", depth * 2, "", child);
            list_flash_tree(child, depth + 1);
        } else {
            ESP_LOGI(TAG, "%*s[FILE] %s (%ld bytes)",
                     depth * 2, "", child, (long)st.st_size);
        }
    }
    closedir(dir);
}

static uint8_t scan_mp3_tracks(void)
{
    if (!usb_msc_is_app_mode()) return 0;
    DIR *dir = opendir(MUSIC_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "opendir('%s') failed: errno=%d (%s)",
                 MUSIC_DIR, errno, strerror(errno));
        return 0;
    }
    uint8_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && strcasecmp(ext, ".mp3") == 0 && count < UINT8_MAX) ++count;
    }
    closedir(dir);
    ESP_LOGI(TAG, "MP3 scan: %u track(s)", count);
    return count;
}

/* Slideshow task - shows images from images/<track>/ while playing */
static void slideshow_task(void *param)
{
    uint8_t prev_track = 0;
    char (*image_paths)[384] = heap_caps_calloc(64, sizeof(*image_paths),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image_paths) image_paths = calloc(64, sizeof(*image_paths));
    if (!image_paths) {
        ESP_LOGE(TAG, "Unable to allocate slideshow path table");
        vTaskDelete(NULL);
        return;
    }
    uint8_t img_count = 0;
    uint8_t img_index = 0;

    while (1) {
        // Skip slideshow if in menu mode
        if (g_menu_state != MENU_NONE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
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
                ESP_LOGI(TAG, "Image %d/%d: %s", img_index + 1, img_count, image_paths[img_index]);
                tft_show_image_file(image_paths[img_index]);
                img_index = (img_index + 1) % img_count;
            }
        } else {
            display_stop();
            prev_track = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* Button task with settings menu support */
static void button_task(void *param)
{
    (void)param;
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);
    gpio_set_direction(GPIO_NUM_43, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_43, GPIO_PULLUP_ONLY);

    uint8_t max_tracks = scan_mp3_tracks();
    uint8_t current = 1;

    while (1) {
        // Handle GPIO0 button
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            TickType_t pressed_at = xTaskGetTickCount();
            bool long_press = false;
            while (gpio_get_level(GPIO_NUM_0) == 0) {
                if ((xTaskGetTickCount() - pressed_at) >= pdMS_TO_TICKS(3000)) {
                    long_press = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            while (gpio_get_level(GPIO_NUM_0) == 0) vTaskDelay(pdMS_TO_TICKS(20));

            // Handle menu navigation
            if (g_menu_state != MENU_NONE) {
                if (g_menu_state == MENU_MAIN) {
                    // Exit menu
                    g_menu_state = MENU_NONE;
                    display_stop();
                } else if (g_menu_state == MENU_VOLUME) {
                    if (long_press) {
                        // Back to main menu
                        g_menu_state = MENU_MAIN;
                        display_settings_menu();
                    } else {
                        // Decrease volume
                        if (g_volume >= 10) g_volume -= 10;
                        else g_volume = 0;
                        apply_volume();
                        save_settings();
                        display_settings_menu();
                    }
                } else if (g_menu_state == MENU_BRIGHTNESS) {
                    if (long_press) {
                        // Back to main menu
                        g_menu_state = MENU_MAIN;
                        display_settings_menu();
                    } else {
                        // Decrease brightness
                        if (g_brightness >= 10) g_brightness -= 10;
                        else g_brightness = 0;
                        apply_brightness();
                        save_settings();
                        display_settings_menu();
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(80));
                continue;
            }

            if (long_press) {
                audio_player_stop();
                vTaskDelay(pdMS_TO_TICKS(100));
                if (usb_msc_is_app_mode()) {
                    ESP_LOGW(TAG, "GPIO0 long press: handing storage to USB host");
                    usb_msc_switch_to_usb();
                } else {
                    ESP_LOGI(TAG, "GPIO0 long press: returning storage to ESP32");
                    if (usb_msc_switch_to_app()) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        ESP_LOGI(TAG, "=== FLASH TREE AFTER APP REMOUNT ===");
                        list_flash_tree(STORAGE_MOUNT_POINT, 0);
                        max_tracks = scan_mp3_tracks();
                        current = 1;
                    }
                }
            } else if (usb_msc_is_app_mode()) {
                max_tracks = scan_mp3_tracks();
                if (max_tracks == 0) {
                    ESP_LOGE(TAG, "Previous: no MP3 files visible");
                } else {
                    current = (current <= 1) ? max_tracks : current - 1;
                    ESP_LOGI(TAG, "Prev track %u", current);
                    audio_player_play_track(current);
                }
            } else {
                ESP_LOGW(TAG, "Storage belongs to USB; long-press GPIO0 to return it to APP");
            }
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }

        // Handle GPIO43 button
        if (gpio_get_level(GPIO_NUM_43) == 0) {
            TickType_t pressed_at = xTaskGetTickCount();
            bool long_press = false;
            while (gpio_get_level(GPIO_NUM_43) == 0) {
                if ((xTaskGetTickCount() - pressed_at) >= pdMS_TO_TICKS(2000)) {
                    long_press = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            while (gpio_get_level(GPIO_NUM_43) == 0) vTaskDelay(pdMS_TO_TICKS(20));

            // Handle menu navigation
            if (g_menu_state != MENU_NONE) {
                if (g_menu_state == MENU_MAIN) {
                    // Enter volume menu
                    g_menu_state = MENU_VOLUME;
                    display_settings_menu();
                } else if (g_menu_state == MENU_VOLUME) {
                    // Increase volume
                    if (g_volume <= 90) g_volume += 10;
                    else g_volume = 100;
                    apply_volume();
                    save_settings();
                    display_settings_menu();
                } else if (g_menu_state == MENU_BRIGHTNESS) {
                    // Increase brightness
                    if (g_brightness <= 90) g_brightness += 10;
                    else g_brightness = 100;
                    apply_brightness();
                    save_settings();
                    display_settings_menu();
                }
                vTaskDelay(pdMS_TO_TICKS(80));
                continue;
            }

            if (long_press) {
                ESP_LOGI(TAG, "GPIO43 long press: diagnostic melody");
                audio_player_stop();
                vTaskDelay(pdMS_TO_TICKS(100));
                audio_player_play_test_tone();
            } else if (usb_msc_is_app_mode()) {
                max_tracks = scan_mp3_tracks();
                if (max_tracks == 0) {
                    ESP_LOGE(TAG, "Next: no MP3 files visible");
                } else {
                    current = (current >= max_tracks) ? 1 : current + 1;
                    ESP_LOGI(TAG, "Next track %u", current);
                    audio_player_play_track(current);
                }
            } else {
                ESP_LOGW(TAG, "Storage belongs to USB; long-press GPIO0 to return it to APP");
            }
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }

        // Both buttons pressed simultaneously -> enter settings menu
        if (gpio_get_level(GPIO_NUM_0) == 0 && gpio_get_level(GPIO_NUM_43) == 0) {
            ESP_LOGI(TAG, "Both buttons pressed: entering settings menu");
            audio_player_stop();
            g_menu_state = MENU_MAIN;
            display_settings_menu();
            // Wait for release
            while (gpio_get_level(GPIO_NUM_0) == 0 || gpio_get_level(GPIO_NUM_43) == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
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

    // Load saved settings
    load_settings();

    // Initialize TFT
    tft_init();
    apply_brightness();

    // Initialize audio
    audio_player_init();
    apply_volume();

    // Initialize USB (CDC ACM + MSC) - also mounts FATFS
    usb_msc_init();
    vTaskDelay(pdMS_TO_TICKS(200));

    // Create default directories after FS is mounted
    mkdir(MUSIC_DIR, 0777);
    mkdir(IMAGE_DIR, 0777);

    ESP_LOGI(TAG, "=== RECURSIVE FLASH TREE: %s ===", STORAGE_MOUNT_POINT);
    list_flash_tree(STORAGE_MOUNT_POINT, 0);
    ESP_LOGI(TAG, "=== END FLASH TREE ===");

    // Test MP3 file
    const char *test_path = MUSIC_DIR "/1.mp3";
    struct stat test_st;
    errno = 0;
    if (stat(test_path, &test_st) == 0) {
        ESP_LOGI(TAG, "stat test OK: %s, %ld bytes", test_path, (long)test_st.st_size);
    } else {
        ESP_LOGE(TAG, "stat test FAIL: %s, errno=%d (%s)",
                 test_path, errno, strerror(errno));
    }

    // Start diagnostic melody
    ESP_LOGI(TAG, "Starting automatic audio diagnostic");
    audio_player_play_test_tone();

    // Show STOP screen
    display_stop();

    // Start slideshow task
    xTaskCreatePinnedToCore(slideshow_task, "slideshow", 4096, NULL, 1, NULL, 1);

    // Start button task
    xTaskCreatePinnedToCore(button_task, "buttons", 3072, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "System ready");
    ESP_LOGI(TAG, "GPIO0 short=Prev, long 3s=APP/USB storage owner");
    ESP_LOGI(TAG, "GPIO43 short=Next, long 2s=diagnostic melody");
    ESP_LOGI(TAG, "Settings: volume=%d%%, brightness=%d%%", g_volume, g_brightness);
}
