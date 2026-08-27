/*
 * ESP32-S3 MP3 Player with LVGL UI + Touch (FT5x06)
 * Branch: lvgl
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
#include "lvgl_port.h"
#include "ui_main.h"

static const char *TAG = "MAIN";

static uint8_t g_volume = 50;
static uint8_t g_brightness = 75;

/* Image cache for preloading */
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

/* Preload images - heap-allocated paths */
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

/* ========== Forward Declarations ========== */
static uint8_t scan_mp3_tracks(void);
static void preload_images_for_track(uint8_t track);

/* ========== UI Callbacks ========== */
static uint8_t max_tracks = 0;
static uint8_t current_track = 1;
static bool is_playing = false;

static void on_prev(void)
{
    if (max_tracks == 0) return;
    current_track = (current_track <= 1) ? max_tracks : current_track - 1;
    ESP_LOGI(TAG, "Prev track %u", current_track);
    preload_images_for_track(current_track);
    audio_player_play_track(current_track);
    is_playing = true;
    if (lvgl_port_lock(100)) {
        ui_show_playing(current_track);
        if (g_image_cache_count > 0) {
            ui_set_image(g_image_cache[0].pixels, g_image_cache[0].width, g_image_cache[0].height);
        }
        lvgl_port_unlock();
    }
}

static void on_play(void)
{
    if (is_playing) {
        audio_player_pause();
        is_playing = false;
        if (lvgl_port_lock(100)) {
            ui_show_stopped();
            lvgl_port_unlock();
        }
    } else {
        if (max_tracks == 0) return;
        preload_images_for_track(current_track);
        audio_player_play_track(current_track);
        is_playing = true;
        if (lvgl_port_lock(100)) {
            ui_show_playing(current_track);
            if (g_image_cache_count > 0) {
                ui_set_image(g_image_cache[0].pixels, g_image_cache[0].width, g_image_cache[0].height);
            }
            lvgl_port_unlock();
        }
    }
}

static void on_next(void)
{
    if (max_tracks == 0) return;
    current_track = (current_track >= max_tracks) ? 1 : current_track + 1;
    ESP_LOGI(TAG, "Next track %u", current_track);
    preload_images_for_track(current_track);
    audio_player_play_track(current_track);
    is_playing = true;
    if (lvgl_port_lock(100)) {
        ui_show_playing(current_track);
        if (g_image_cache_count > 0) {
            ui_set_image(g_image_cache[0].pixels, g_image_cache[0].width, g_image_cache[0].height);
        }
        lvgl_port_unlock();
    }
}

static void on_volume_change(uint8_t vol)
{
    g_volume = vol;
    apply_volume();
    save_settings();
}

static void on_brightness_change(uint8_t bright)
{
    g_brightness = bright;
    apply_brightness();
    save_settings();
}

static void on_usb_mode(void)
{
    ESP_LOGI(TAG, "USB mode switch requested");
    audio_player_stop();
    is_playing = false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (usb_msc_is_app_mode()) {
        usb_msc_switch_to_usb();
    } else {
        if (usb_msc_switch_to_app()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            max_tracks = scan_mp3_tracks();
            current_track = 1;
            preload_images_for_track(1);
        }
    }
    /* Return to menu after USB switch */
    if (lvgl_port_lock(100)) {
        ui_show_menu();
        lvgl_port_unlock();
    }
}

/* ========== I2C Command Queue ========== */
typedef struct {
    uint8_t type; // 1=play, 2=stop, 3=volume, 4=brightness
    uint8_t value;
} i2c_cmd_t;

static QueueHandle_t i2c_cmd_queue = NULL;

/* ========== I2C Callbacks (lightweight - just enqueue) ========== */
static void i2c_play_track_callback(uint8_t track)
{
    ESP_LOGI(TAG, "I2C: Play track %d (queued)", track);
    i2c_cmd_t cmd = { .type = 1, .value = track };
    if (i2c_cmd_queue) xQueueSend(i2c_cmd_queue, &cmd, 0);
}

static void i2c_stop_callback(void)
{
    ESP_LOGI(TAG, "I2C: Stop (queued)");
    i2c_cmd_t cmd = { .type = 2, .value = 0 };
    if (i2c_cmd_queue) xQueueSend(i2c_cmd_queue, &cmd, 0);
}

static void i2c_volume_callback(uint8_t volume)
{
    ESP_LOGI(TAG, "I2C: Volume %d (queued)", volume);
    i2c_cmd_t cmd = { .type = 3, .value = volume };
    if (i2c_cmd_queue) xQueueSend(i2c_cmd_queue, &cmd, 0);
}

static void i2c_brightness_callback(uint8_t brightness)
{
    ESP_LOGI(TAG, "I2C: Brightness %d (queued)", brightness);
    i2c_cmd_t cmd = { .type = 4, .value = brightness };
    if (i2c_cmd_queue) xQueueSend(i2c_cmd_queue, &cmd, 0);
}

/* ========== I2C Command Handler Task ========== */
static void i2c_cmd_task(void *param)
{
    (void)param;
    i2c_cmd_t cmd;

    while (1) {
        if (xQueueReceive(i2c_cmd_queue, &cmd, portMAX_DELAY)) {
            switch (cmd.type) {
                case 1: { // Play track
                    uint8_t track = cmd.value;
                    ESP_LOGI(TAG, "I2C CMD: Play track %d", track);
                    current_track = track;
                    preload_images_for_track(track);
                    audio_player_play_track(track);
                    is_playing = true;
                    if (lvgl_port_lock(100)) {
                        ui_show_playing(track);
                        if (g_image_cache_count > 0) {
                            ui_set_image(g_image_cache[0].pixels, g_image_cache[0].width, g_image_cache[0].height);
                        }
                        lvgl_port_unlock();
                    }
                    break;
                }
                case 2: { // Stop
                    ESP_LOGI(TAG, "I2C CMD: Stop");
                    audio_player_stop();
                    is_playing = false;
                    if (lvgl_port_lock(100)) {
                        ui_show_stopped();
                        lvgl_port_unlock();
                    }
                    break;
                }
                case 3: { // Volume
                    ESP_LOGI(TAG, "I2C CMD: Volume %d", cmd.value);
                    g_volume = cmd.value;
                    apply_volume();
                    save_settings();
                    if (lvgl_port_lock(100)) {
                        ui_set_volume(cmd.value);
                        lvgl_port_unlock();
                    }
                    break;
                }
                case 4: { // Brightness
                    ESP_LOGI(TAG, "I2C CMD: Brightness %d", cmd.value);
                    g_brightness = cmd.value;
                    apply_brightness();
                    save_settings();
                    if (lvgl_port_lock(100)) {
                        ui_set_brightness(cmd.value);
                        lvgl_port_unlock();
                    }
                    break;
                }
            }
        }
    }
}

/* ========== Slideshow Task ========== */
static void slideshow_task(void *param)
{
    int img_index = 0;
    uint8_t prev_track = 0;

    while (1) {
        if (is_playing && audio_player_get_state() == PLAYER_STATE_PLAYING) {
            uint8_t track = audio_player_get_current_track();
            if (track != prev_track) {
                prev_track = track;
                img_index = 0;
                i2c_slave_set_current_track(track);
                i2c_slave_set_status(STATUS_PLAYING);
            }

            if (g_image_cache_count > 0 && g_image_cache[img_index].pixels) {
                if (lvgl_port_lock(100)) {
                    ui_set_image(g_image_cache[img_index].pixels,
                                 g_image_cache[img_index].width,
                                 g_image_cache[img_index].height);
                    lvgl_port_unlock();
                }
                img_index = (img_index + 1) % g_image_cache_count;
            }
        } else {
            prev_track = 0;
            i2c_slave_set_status(STATUS_STOPPED);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
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

/* ========== Menu Action Wrappers (for touch click) ========== */
static void menu_play_action(void)
{
    if (max_tracks > 0) {
        preload_images_for_track(current_track);
        audio_player_play_track(current_track);
        is_playing = true;
        lvgl_port_lock(100);
        ui_show_playing(current_track);
        if (g_image_cache_count > 0) {
            ui_set_image(g_image_cache[0].pixels, g_image_cache[0].width, g_image_cache[0].height);
        }
        lvgl_port_unlock();
    }
}

static void menu_settings_action(void)
{
    lvgl_port_lock(100);
    ui_show_settings();
    lvgl_port_unlock();
}

/* ========== Button Task (GPIO fallback) ========== */
static void button_task(void *param)
{
    (void)param;
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);
    gpio_set_direction(GPIO_NUM_43, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_43, GPIO_PULLUP_ONLY);

    const TickType_t LONG_PRESS_MS = 2000; /* 2 seconds for long press */

    while (1) {
        /* ===== GPIO0 (Left Button): Back / Prev / Menu Navigate ===== */
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            TickType_t pressed_at = xTaskGetTickCount();
            bool long_press = false;
            while (gpio_get_level(GPIO_NUM_0) == 0) {
                if ((xTaskGetTickCount() - pressed_at) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
                    long_press = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            while (gpio_get_level(GPIO_NUM_0) == 0) vTaskDelay(pdMS_TO_TICKS(20));

            ui_state_t state = ui_get_state();

            if (long_press) {
                /* Long press GPIO0: Return to main menu from any screen */
                if (state != UI_STATE_MENU && state != UI_STATE_WELCOME) {
                    if (lvgl_port_lock(100)) {
                        ui_hide_settings(); /* Close settings if open */
                        ui_show_menu();
                        lvgl_port_unlock();
                    }
                }
            } else {
                /* Short press GPIO0: Context-aware back/prev */
                if (lvgl_port_lock(100)) {
                    if (state == UI_STATE_SETTINGS) {
                        /* In settings: decrease value or switch focus */
                        ui_settings_adjust(-1);
                    } else if (state == UI_STATE_MENU) {
                        /* In menu: navigate up */
                        ui_menu_navigate(-1);
                    } else if (state == UI_STATE_PLAYING || state == UI_STATE_STOPPED) {
                        /* Playing: previous track */
                        lvgl_port_unlock();
                        on_prev();
                        continue; /* Skip the unlock below */
                    } else if (state == UI_STATE_WELCOME) {
                        /* Welcome: go to menu */
                        ui_show_menu();
                    }
                    lvgl_port_unlock();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        /* ===== GPIO43 (Right Button): Confirm / Next / Settings ===== */
        if (gpio_get_level(GPIO_NUM_43) == 0) {
            TickType_t pressed_at = xTaskGetTickCount();
            bool long_press = false;
            while (gpio_get_level(GPIO_NUM_43) == 0) {
                if ((xTaskGetTickCount() - pressed_at) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
                    long_press = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            while (gpio_get_level(GPIO_NUM_43) == 0) vTaskDelay(pdMS_TO_TICKS(20));

            ui_state_t state = ui_get_state();

            if (long_press) {
                /* Long press GPIO43: Toggle settings panel */
                if (lvgl_port_lock(100)) {
                    if (state == UI_STATE_SETTINGS) {
                        ui_hide_settings();
                    } else if (state != UI_STATE_WELCOME) {
                        ui_show_settings();
                    }
                    lvgl_port_unlock();
                }
            } else {
                /* Short press GPIO43: Confirm / Next / Navigate down */
                if (lvgl_port_lock(100)) {
                    if (state == UI_STATE_SETTINGS) {
                        /* In settings: increase value */
                        ui_settings_adjust(1);
                    } else if (state == UI_STATE_MENU) {
                        /* In menu: confirm selection */
                        lvgl_port_unlock();
                        menu_item_t sel = ui_menu_confirm();
                        if (sel == MENU_ITEM_PLAY) {
                            if (max_tracks > 0) {
                                preload_images_for_track(current_track);
                                audio_player_play_track(current_track);
                                is_playing = true;
                                if (lvgl_port_lock(100)) {
                                    ui_show_playing(current_track);
                                    if (g_image_cache_count > 0) {
                                        ui_set_image(g_image_cache[0].pixels, g_image_cache[0].width, g_image_cache[0].height);
                                    }
                                    lvgl_port_unlock();
                                }
                            }
                        } else if (sel == MENU_ITEM_USB) {
                            on_usb_mode();
                        } else if (sel == MENU_ITEM_SETTINGS) {
                            if (lvgl_port_lock(100)) {
                                ui_show_settings();
                                lvgl_port_unlock();
                            }
                        }
                        continue; /* Skip the unlock below */
                    } else if (state == UI_STATE_PLAYING || state == UI_STATE_STOPPED) {
                        /* Playing: next track */
                        lvgl_port_unlock();
                        on_next();
                        continue; /* Skip the unlock below */
                    } else if (state == UI_STATE_WELCOME) {
                        /* Welcome: go to menu */
                        ui_show_menu();
                    }
                    lvgl_port_unlock();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== LVGL MP3 Player ===");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_settings();

    /* Initialize audio */
    audio_player_init();
    apply_volume();

    /* Play diagnostic test tone BEFORE LVGL starts (no LVGL task interference) */
    audio_player_play_test_tone();

    /* Initialize TFT */
    tft_init();
    tft_fill_screen(0x0000);
    apply_brightness();

    /* Initialize LVGL */
    lvgl_port_init();
    ui_init();

    /* Set UI callbacks */
    ui_set_callbacks(on_prev, on_play, on_next, on_volume_change, on_brightness_change, on_usb_mode);

    /* Set menu item callbacks for touch click */
    ui_set_menu_callbacks(menu_play_action, on_usb_mode, menu_settings_action);

    /* Show welcome screen */
    ui_show_welcome();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Create I2C command queue (depth=10) */
    i2c_cmd_queue = xQueueCreate(10, sizeof(i2c_cmd_t));
    if (!i2c_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create I2C command queue");
        while (1) vTaskDelay(portMAX_DELAY);
    }

    /* Initialize I2C Slave */
    i2c_slave_init();
    i2c_slave_set_callbacks(i2c_play_track_callback, i2c_stop_callback,
                            i2c_volume_callback, i2c_brightness_callback);
    i2c_slave_set_volume(g_volume);
    i2c_slave_set_brightness(g_brightness);

    /* Initialize USB MSC */
    usb_msc_init();
    vTaskDelay(pdMS_TO_TICKS(200));

    mkdir(MUSIC_DIR, 0777);
    mkdir(IMAGE_DIR, 0777);

    ESP_LOGI(TAG, "=== FLASH TREE ===");
    list_flash_tree(STORAGE_MOUNT_POINT, 0);
    ESP_LOGI(TAG, "=== END FLASH TREE ===");

    /* Scan tracks and preload */
    max_tracks = scan_mp3_tracks();
    ESP_LOGI(TAG, "Found %d MP3 tracks", max_tracks);

    if (max_tracks > 0) {
        preload_images_for_track(1);
    }

    /* Show main menu */
    ui_show_menu();

    /* Create tasks */
    xTaskCreatePinnedToCore(slideshow_task, "slideshow", 8192, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(button_task, "buttons", 32768, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(i2c_cmd_task, "i2c_cmd", 8192, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "System ready");
    ESP_LOGI(TAG, "GPIO0: Up/Back | Long=USB  GPIO43: Down/Enter | Long=Settings");
    ESP_LOGI(TAG, "Touch: FT5x06 at 0x%02X", TOUCH_I2C_ADDR);
    ESP_LOGI(TAG, "I2C Slave: 0x%02X", I2C_SLAVE_ADDR);
    ESP_LOGI(TAG, "Settings: vol=%d%%, bright=%d%%", g_volume, g_brightness);
}