/*
 * ESP32-S3 MP3 Player with I2C Slave Control
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "config.h"
#include "tft_driver.h"
#include "audio_player.h"
#include "fatfs_manager.h"
#include "usb_msc.h"
#include "image_decoder.h"
#include "i2c_slave.h"

static const char *TAG = "MAIN";

static uint8_t g_volume = 50;
static uint8_t g_brightness = 75;

typedef enum {
    MENU_NONE,
    MENU_MAIN,
    MENU_VOLUME,
    MENU_BRIGHTNESS
} menu_state_t;

static menu_state_t g_menu_state = MENU_NONE;

#define MAX_PRELOAD_IMAGES 16
static decoded_image_t g_image_cache[MAX_PRELOAD_IMAGES];
static int g_image_cache_count = 0;
static uint8_t g_cached_track = 0;

static bool is_image_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".bmp") == 0) return true;
    if (strcasecmp(ext, ".jpg") == 0) return true;
    if (strcasecmp(ext, ".jpeg") == 0) return true;
    return false;
}

static void load_settings(void)
{
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "volume", &g_volume);
        nvs_get_u8(nvs, "brightness", &g_brightness);
        nvs_close(nvs);
    }
}

static void save_settings(void)
{
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "volume", g_volume);
        nvs_set_u8(nvs, "brightness", g_brightness);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void apply_volume(void)
{
    audio_player_set_volume(g_volume);
    i2c_slave_set_volume(g_volume);
}

static void apply_brightness(void)
{
    tft_set_brightness(g_brightness);
    i2c_slave_set_brightness(g_brightness);
}

static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44}
};

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

static void draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color)
{
    while (*str) {
        draw_char(x, y, *str, color);
        x += 12;
        str++;
    }
}

static void draw_progress_bar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t percent, uint16_t color)
{
    for (uint16_t i = 0; i < w; i++) {
        tft_draw_pixel(x + i, y, color);
        tft_draw_pixel(x + i, y + h - 1, color);
    }
    for (uint16_t i = 0; i < h; i++) {
        tft_draw_pixel(x, y + i, color);
        tft_draw_pixel(x + w - 1, y + i, color);
    }
    uint16_t fill_w = (w - 2) * percent / 100;
    for (uint16_t i = 0; i < fill_w; i++) {
        for (uint16_t j = 1; j < h - 1; j++) {
            tft_draw_pixel(x + 1 + i, y + j, color);
        }
    }
}

static void display_stop(void)
{
    tft_fill_screen(0x0000);
    draw_string(72, 140, "STOP", 0xF800);
    draw_string(36, 200, "GPIO43: Play", 0x07FF);
}

static void display_playing(uint8_t track)
{
    tft_fill_screen(0x0000);
    draw_string(12, 10, "Now Playing", 0x07FF);
    { char buf[16]; snprintf(buf, sizeof(buf), "Track %d", track);
      draw_string(12, 40, buf, 0xFFFF); }
}

static void display_settings_menu(void)
{
    tft_fill_screen(0x0000);
    switch (g_menu_state) {
        case MENU_MAIN:
            draw_string(60, 20, "SETTINGS", 0xFFFF);
            draw_string(20, 60, "1. Volume", 0x07FF);
            draw_string(20, 100, "2. Brightness", 0x07FF);
            draw_string(20, 160, "GPIO43: Select", 0xFFFF);
            draw_string(20, 200, "GPIO0: Exit", 0xF800);
            break;
        case MENU_VOLUME:
            draw_string(60, 20, "VOLUME", 0xFFFF);
            draw_string(20, 60, "GPIO43: +10", 0x07FF);
            draw_string(20, 100, "GPIO0: -10", 0x07FF);
            draw_string(20, 160, "Value:", 0xFFFF);
            { char s[8]; snprintf(s, sizeof(s), "%d%%", g_volume); draw_string(100, 160, s, 0x07FF); }
            draw_progress_bar(20, 200, 200, 20, g_volume, 0x07FF);
            draw_string(20, 260, "Long GPIO0: Back", 0xF800);
            break;
        case MENU_BRIGHTNESS:
            draw_string(40, 20, "BRIGHTNESS", 0xFFFF);
            draw_string(20, 60, "GPIO43: +10", 0x07FF);
            draw_string(20, 100, "GPIO0: -10", 0x07FF);
            draw_string(20, 160, "Value:", 0xFFFF);
            { char s[8]; snprintf(s, sizeof(s), "%d%%", g_brightness); draw_string(100, 160, s, 0x07FF); }
            draw_progress_bar(20, 200, 200, 20, g_brightness, 0xFFFF);
            draw_string(20, 260, "Long GPIO0: Back", 0xF800);
            break;
        default: break;
    }
}

#define FILE_LIST_MAX_DEPTH 6
static void list_flash_tree(const char *path, int depth)
{
    if (depth > FILE_LIST_MAX_DEPTH) return;
    errno = 0;
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[384];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || n >= (int)sizeof(child)) continue;
        struct stat st;
        errno = 0;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            ESP_LOGI(TAG, "%*s[DIR ] %s", depth * 2, "", child);
            list_flash_tree(child, depth + 1);
        } else {
            ESP_LOGI(TAG, "%*s[FILE] %s (%ld bytes)", depth * 2, "", child, (long)st.st_size);
        }
    }
    closedir(dir);
}

static uint8_t scan_mp3_tracks(void)
{
    if (!usb_msc_is_app_mode()) return 0;
    DIR *dir = opendir(MUSIC_DIR);
    if (!dir) return 0;
    uint8_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && strcasecmp(ext, ".mp3") == 0 && count < UINT8_MAX) ++count;
    }
    closedir(dir);
    return count;
}

static void clear_image_cache(void)
{
    for (int i = 0; i < g_image_cache_count; i++) {
        if (g_image_cache[i].pixels) {
            free(g_image_cache[i].pixels);
            g_image_cache[i].pixels = NULL;
        }
    }
    g_image_cache_count = 0;
    g_cached_track = 0;
}

/* Preload images - heap-allocated paths to avoid stack overflow */
static void preload_images_for_track(uint8_t track)
{
    if (track == g_cached_track && g_image_cache_count > 0) return;
    clear_image_cache();

    char track_dir[64];
    snprintf(track_dir, sizeof(track_dir), "%s/%d", IMAGE_DIR, track);
    
    DIR *dir = opendir(track_dir);
    if (!dir) { ESP_LOGW(TAG, "No image dir: %s", track_dir); return; }

    ESP_LOGI(TAG, "Preloading images from %s...", track_dir);
    int64_t start_ms = esp_timer_get_time() / 1000;

    // Heap-allocated paths to avoid 6KB stack usage
    char **paths = calloc(MAX_PRELOAD_IMAGES, sizeof(char*));
    if (!paths) { closedir(dir); return; }

    int path_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && path_count < MAX_PRELOAD_IMAGES) {
        if (is_image_file(entry->d_name)) {
            paths[path_count] = malloc(384);
            if (paths[path_count]) {
                snprintf(paths[path_count], 384, "%s/%s", track_dir, entry->d_name);
                path_count++;
            }
        }
    }
    closedir(dir);

    for (int i = 0; i < path_count; i++) {
        ESP_LOGI(TAG, "  Decoding %d/%d: %s", i + 1, path_count, paths[i]);
        if (decode_image_file(paths[i], DISPLAY_WIDTH, DISPLAY_HEIGHT, &g_image_cache[g_image_cache_count])) {
            g_image_cache_count++;
        } else {
            ESP_LOGW(TAG, "  Failed: %s", paths[i]);
        }
        free(paths[i]);
    }
    free(paths);

    int64_t elapsed_ms = esp_timer_get_time() / 1000 - start_ms;
    ESP_LOGI(TAG, "Preload complete: %d images in %lld ms", g_image_cache_count, elapsed_ms);
    g_cached_track = track;
}

/* Slideshow: only reads from cache, no preloading */
static void slideshow_task(void *param)
{
    uint8_t prev_track = 0;
    int img_index = 0;

    while (1) {
        if (g_menu_state != MENU_NONE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        if (audio_player_get_state() == PLAYER_STATE_PLAYING) {
            uint8_t track = audio_player_get_current_track();

            if (track != prev_track) {
                prev_track = track;
                img_index = 0;
                display_playing(track);
                i2c_slave_set_current_track(track);
                i2c_slave_set_status(STATUS_PLAYING);
            }

            if (g_image_cache_count > 0) {
                decoded_image_t *img = &g_image_cache[img_index];
                if (img->pixels) {
                    tft_show_rgb565(img->pixels, img->width, img->height);
                }
                img_index = (img_index + 1) % g_image_cache_count;
            } else {
                // No images available - show placeholder
                display_playing(track);
                draw_string(24, 140, "No image found", 0xF800);
            }
        } else {
            display_stop();
            prev_track = 0;
            i2c_slave_set_status(STATUS_STOPPED);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void i2c_play_track_callback(uint8_t track)
{
    ESP_LOGI(TAG, "I2C: Play track %d", track);
    preload_images_for_track(track);
    audio_player_play_track(track);
}

static void i2c_stop_callback(void) { audio_player_stop(); }

static void i2c_volume_callback(uint8_t volume)
{
    g_volume = volume; apply_volume(); save_settings();
}

static void i2c_brightness_callback(uint8_t brightness)
{
    g_brightness = brightness; apply_brightness(); save_settings();
}

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
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            TickType_t pressed_at = xTaskGetTickCount();
            bool long_press = false;
            while (gpio_get_level(GPIO_NUM_0) == 0) {
                if ((xTaskGetTickCount() - pressed_at) >= pdMS_TO_TICKS(3000)) { long_press = true; break; }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            while (gpio_get_level(GPIO_NUM_0) == 0) vTaskDelay(pdMS_TO_TICKS(20));

            if (g_menu_state != MENU_NONE) {
                if (g_menu_state == MENU_MAIN) { g_menu_state = MENU_NONE; display_stop(); }
                else if (g_menu_state == MENU_VOLUME) {
                    if (long_press) { g_menu_state = MENU_MAIN; display_settings_menu(); }
                    else { g_volume = (g_volume >= 10) ? g_volume - 10 : 0; apply_volume(); save_settings(); display_settings_menu(); }
                }
                else if (g_menu_state == MENU_BRIGHTNESS) {
                    if (long_press) { g_menu_state = MENU_MAIN; display_settings_menu(); }
                    else { g_brightness = (g_brightness >= 10) ? g_brightness - 10 : 0; apply_brightness(); save_settings(); display_settings_menu(); }
                }
                vTaskDelay(pdMS_TO_TICKS(80)); continue;
            }

            if (long_press) {
                audio_player_stop();
                vTaskDelay(pdMS_TO_TICKS(100));
                if (usb_msc_is_app_mode()) { usb_msc_switch_to_usb(); }
                else {
                    if (usb_msc_switch_to_app()) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        list_flash_tree(STORAGE_MOUNT_POINT, 0);
                        max_tracks = scan_mp3_tracks();
                        current = 1;
                        preload_images_for_track(1);
                    }
                }
            } else if (usb_msc_is_app_mode()) {
                max_tracks = scan_mp3_tracks();
                if (max_tracks > 0) {
                    current = (current <= 1) ? max_tracks : current - 1;
                    ESP_LOGI(TAG, "Prev track %u", current);
                    preload_images_for_track(current);
                    audio_player_play_track(current);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(80)); continue;
        }

        if (gpio_get_level(GPIO_NUM_43) == 0) {
            TickType_t pressed_at = xTaskGetTickCount();
            bool long_press = false;
            while (gpio_get_level(GPIO_NUM_43) == 0) {
                if ((xTaskGetTickCount() - pressed_at) >= pdMS_TO_TICKS(2000)) { long_press = true; break; }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            while (gpio_get_level(GPIO_NUM_43) == 0) vTaskDelay(pdMS_TO_TICKS(20));

            if (g_menu_state != MENU_NONE) {
                if (g_menu_state == MENU_MAIN) { g_menu_state = MENU_VOLUME; display_settings_menu(); }
                else if (g_menu_state == MENU_VOLUME) {
                    g_volume = (g_volume <= 90) ? g_volume + 10 : 100; apply_volume(); save_settings(); display_settings_menu();
                }
                else if (g_menu_state == MENU_BRIGHTNESS) {
                    g_brightness = (g_brightness <= 90) ? g_brightness + 10 : 100; apply_brightness(); save_settings(); display_settings_menu();
                }
                vTaskDelay(pdMS_TO_TICKS(80)); continue;
            }

            if (long_press) {
                audio_player_stop();
                g_menu_state = MENU_MAIN;
                display_settings_menu();
            } else if (usb_msc_is_app_mode()) {
                max_tracks = scan_mp3_tracks();
                if (max_tracks > 0) {
                    current = (current >= max_tracks) ? 1 : current + 1;
                    ESP_LOGI(TAG, "Next track %u", current);
                    preload_images_for_track(current);
                    audio_player_play_track(current);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(80)); continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Minimal MP3 Player ===");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_settings();
    tft_init();

    // Welcome screen
    tft_fill_screen(0x001F);
    draw_string(60, 100, "MP3 Player", 0xFFFF);
    draw_string(40, 140, "Initializing...", 0x07FF);
    vTaskDelay(pdMS_TO_TICKS(1000));

    apply_brightness();
    audio_player_init();
    apply_volume();

    i2c_slave_init();
    i2c_slave_set_callbacks(i2c_play_track_callback, i2c_stop_callback, i2c_volume_callback, i2c_brightness_callback);
    i2c_slave_set_volume(g_volume);
    i2c_slave_set_brightness(g_brightness);

    usb_msc_init();
    vTaskDelay(pdMS_TO_TICKS(200));

    mkdir(MUSIC_DIR, 0777);
    mkdir(IMAGE_DIR, 0777);

    ESP_LOGI(TAG, "=== FLASH TREE: %s ===", STORAGE_MOUNT_POINT);
    list_flash_tree(STORAGE_MOUNT_POINT, 0);
    ESP_LOGI(TAG, "=== END FLASH TREE ===");

    // Preload track 1 images at boot
    preload_images_for_track(1);

    // Diagnostic tone
    audio_player_play_test_tone();

    display_stop();

    // Slideshow task with larger stack (no heavy stack allocations)
    xTaskCreatePinnedToCore(slideshow_task, "slideshow", 8192, NULL, 1, NULL, 1);

    // Button task
    xTaskCreatePinnedToCore(button_task, "buttons", 16384, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "System ready");
    ESP_LOGI(TAG, "GPIO0 short=Prev, long 3s=USB");
    ESP_LOGI(TAG, "GPIO43 short=Next, long 2s=Settings");
    ESP_LOGI(TAG, "I2C Slave: 0x%02X", I2C_SLAVE_ADDR);
    ESP_LOGI(TAG, "Settings: vol=%d%%, bright=%d%%", g_volume, g_brightness);
}
