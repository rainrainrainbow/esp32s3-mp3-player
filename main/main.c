/*
 * main.c - ESP32-S3 MP3 Player with I2C slave control
 *
 * Hardware: ESP32-S3 N16R8, OPI PSRAM, ES8311, 240x320 TFT
 * 
 * Features:
 * - I2C slave at 0x52, registers: 0x01=track, 0x02=status
 * - MP3 playback from SPI flash FATFS via ES8311
 * - TFT image display (BMP slideshow)
 * - USB Mass Storage for file transfer
 * - 音画同步: 播放 music/N.mp3 时轮播 images/N/ 下的图片
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
#include "i2c_slave.h"
#include "usb_msc.h"
#include "fatfs_manager.h"
#include "image_decoder.h"

static const char *TAG = "MAIN";

/* Check if filename has an image extension */
static bool is_image_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".bmp") == 0) return true;
    return false;
}

/* Image slideshow task - shows images from images/<track>/ folder matching current track */
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
                // Track changed - scan the new image folder
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
                    ESP_LOGI(TAG, "Track %d: found %d images in %s", track, img_count, track_dir);
                } else {
                    ESP_LOGW(TAG, "No image folder for track %d: %s", track, track_dir);
                }
            }

            if (img_count > 0) {
                ESP_LOGI(TAG, "Showing image %d/%d: %s", img_index + 1, img_count, image_paths[img_index]);
                tft_show_image_file(image_paths[img_index]);
                img_index = (img_index + 1) % img_count;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* I2C status update task */
static void status_update_task(void *param)
{
    while (1) {
        i2c_slave_write_reg(REG_PLAY_STATUS, (uint8_t)audio_player_get_state());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Button task - GPIO0 prev, GPIO43 next */
#define DEBOUNCE_MS 50
static void button_task(void *param)
{
    uint8_t max_tracks = 0;

    // Count tracks on startup
    DIR *dir = opendir(MUSIC_DIR);
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

    // GPIO0 = BOOT button (short press=prev, long press>3s=USB mode)
    // GPIO43 = next track
    gpio_set_direction(LEFT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(LEFT_BUTTON_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_direction(RIGHT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(RIGHT_BUTTON_GPIO, GPIO_PULLUP_ONLY);

    uint8_t current = 1;
    while (1) {
        bool left_pressed = (gpio_get_level(LEFT_BUTTON_GPIO) == 0);
        bool right_pressed = (gpio_get_level(RIGHT_BUTTON_GPIO) == 0);

        if (left_pressed) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(LEFT_BUTTON_GPIO) == 0) {
                // Wait to distinguish short/long press
                uint32_t press_ms = 0;
                while (gpio_get_level(LEFT_BUTTON_GPIO) == 0 && press_ms < 3000) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    press_ms += 10;
                }
                if (press_ms >= 3000) {
                    // Long press: switch to USB mode
                    ESP_LOGI(TAG, "Long press: USB storage mode");
                    usb_msc_switch_to_usb();
                } else {
                    // Short press: previous track
                    current = (current == 1) ? max_tracks : current - 1;
                    ESP_LOGI(TAG, "Prev track %d", current);
                    audio_player_play_track(current);
                }
                while (gpio_get_level(LEFT_BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        if (right_pressed) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(RIGHT_BUTTON_GPIO) == 0) {
                current = (current >= max_tracks) ? 1 : current + 1;
                ESP_LOGI(TAG, "Next track %d", current);
                audio_player_play_track(current);
                while (gpio_get_level(RIGHT_BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3 MP3 Player Starting...");
    ESP_LOGI(TAG, "Board: ESP32-S3 N16R8 + OPI PSRAM");
    ESP_LOGI(TAG, "Audio: ES8311 via I2S");
    ESP_LOGI(TAG, "Display: 240x320 TFT SPI");
    ESP_LOGI(TAG, "I2C Slave: 0x%02X", I2C_SLAVE_ADDR);
    ESP_LOGI(TAG, "========================================");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TFT display
    tft_init();
    tft_fill_screen(0x0000);

    // Initialize audio player
    audio_player_init();

    // Mount SPI flash FATFS
    if (!fatfs_mount_spiflash()) {
        ESP_LOGE(TAG, "FATFS mount failed - check partition table");
        tft_fill_screen(0xF800);
    }

    // Initialize I2C slave
    i2c_slave_init();

    // Initialize USB MSC (U盘)
    usb_msc_init();

    // Show startup screen (blue)
    tft_fill_screen(0x001F);
    vTaskDelay(pdMS_TO_TICKS(1000));
    tft_fill_screen(0x0000);

    // Start slideshow task (音画同步: images/<track>/ → music/<track>.mp3)
    xTaskCreatePinnedToCore(slideshow_task, "slideshow", 4096, NULL, 1, NULL, 1);

    // Start status update task
    xTaskCreatePinnedToCore(status_update_task, "status_upd", 2048, NULL, 1, NULL, 1);

    // Start button task (GPIO0=prev, GPIO43=next)
    xTaskCreatePinnedToCore(button_task, "buttons", 3072, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "System ready");
    ESP_LOGI(TAG, " - 音画同步: music/N.mp3 + images/N/ 图片轮播");
    ESP_LOGI(TAG, " - I2C: Write 0x01 <track> to play");
    ESP_LOGI(TAG, " - I2C: Read 0x02 for status");
    ESP_LOGI(TAG, " - GPIO43: Next track");
    ESP_LOGI(TAG, " - GPIO0 short=Prev, long>3s=USB mode");
}
