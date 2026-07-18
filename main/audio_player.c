/*
 * audio_player.c - ES8311 audio playback via I2S + minimp3 decoder
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s.h"
#include "driver/i2c.h"
#include "config.h"
#include "audio_player.h"

// minimp3 decoder
#include "minimp3.h"

static const char *TAG = "AUDIO";

static player_state_t player_state = PLAYER_STATE_STOPPED;
static uint8_t current_track = 0;
static TaskHandle_t audio_task_handle = NULL;
static volatile bool audio_task_running = false;

static i2s_chan_handle_t i2s_tx_handle = NULL;

// ES8311 I2C address
#define ES8311_I2C_ADDR 0x18

static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_1, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void es8311_init(void)
{
    ESP_LOGI(TAG, "Initializing ES8311");

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_1, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_1, I2C_MODE_MASTER, 0, 0, 0));

    // Reset ES8311
    es8311_write_reg(0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Clock manager: MCLK from GPIO
    es8311_write_reg(0x01, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Power up DAC, soft ramp
    es8311_write_reg(0x1F, 0x24);
    vTaskDelay(pdMS_TO_TICKS(10));

    // I2S format: 16-bit, I2S standard
    es8311_write_reg(0x0A, 0x00); // SDP_IN
    es8311_write_reg(0x0B, 0x00); // SDP_OUT

    // DAC volume (0dB)
    es8311_write_reg(0x22, 0x00); // DAC_VOL_L
    es8311_write_reg(0x23, 0x00); // DAC_VOL_R

    // HP output from DAC
    es8311_write_reg(0x26, 0x05);
    es8311_write_reg(0x27, 0x1E);
    es8311_write_reg(0x28, 0x00);

    // PA enable
    gpio_set_direction(AUDIO_CODEC_PA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_CODEC_PA_PIN, 1);

    ESP_LOGI(TAG, "ES8311 ready");
}

static void i2s_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S");

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear = true,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_GPIO_MCLK,
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_WS,
            .dout = AUDIO_I2S_GPIO_DOUT,
            .din = AUDIO_I2S_GPIO_DIN,
            .invert_flags = {false, false, false},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_handle));
}

/* MP3 playback task using minimp3 */
static void audio_playback_task(void *param)
{
    uint8_t track = (uint32_t)param;
    char path[64];
    snprintf(path, sizeof(path), "%s/%03d.mp3", MUSIC_DIR, track);

    ESP_LOGI(TAG, "Opening: %s", path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open MP3: %s", path);
        player_state = PLAYER_STATE_STOPPED;
        audio_task_running = false;
        audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *mp3_buf = malloc(fsize);
    if (!mp3_buf) {
        ESP_LOGE(TAG, "OOM (%ld bytes)", fsize);
        fclose(f);
        player_state = PLAYER_STATE_STOPPED;
        audio_task_running = false;
        audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    fread(mp3_buf, 1, fsize, f);
    fclose(f);

    // Initialize minimp3 decoder
    mp3dec_t dec;
    mp3dec_init(&dec);

    mp3dec_frame_info_t info;
    int16_t pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];

    uint8_t *input = mp3_buf;
    int bytes_left = fsize;

    player_state = PLAYER_STATE_PLAYING;
    audio_task_running = true;

    while (bytes_left > 0 && player_state == PLAYER_STATE_PLAYING) {
        int samples = mp3dec_decode_frame(&dec, input, bytes_left, pcm_buf, &info);
        if (info.frame_bytes > 0) {
            input += info.frame_bytes;
            bytes_left -= info.frame_bytes;
        } else {
            // No frame found, advance by 1 byte
            input++;
            bytes_left--;
            continue;
        }

        if (samples > 0) {
            // Convert to mono if stereo
            int channels = info.channels;
            int frames = samples / channels;

            if (channels == 2 && frames > 0) {
                // Simple mono downmix to local buffer
                int16_t mono[1152];
                for (int i = 0; i < frames && i < 1152; i++) {
                    int32_t sum = (int32_t)pcm_buf[i * 2] + (int32_t)pcm_buf[i * 2 + 1];
                    mono[i] = sum >> 1;
                }
                size_t written = 0;
                i2s_channel_write(i2s_tx_handle, mono, frames * sizeof(int16_t), &written, portMAX_DELAY);
            } else {
                size_t written = 0;
                i2s_channel_write(i2s_tx_handle, pcm_buf, samples * sizeof(int16_t), &written, portMAX_DELAY);
            }
        }
    }

    ESP_LOGI(TAG, "Playback finished");
    free(mp3_buf);
    player_state = PLAYER_STATE_STOPPED;
    audio_task_running = false;
    audio_task_handle = NULL;
    vTaskDelete(NULL);
}

void audio_player_init(void)
{
    ESP_LOGI(TAG, "Audio player init");
    i2s_init();
    es8311_init();
    ESP_LOGI(TAG, "Audio player ready");
}

bool audio_player_play_track(uint8_t track_num)
{
    if (track_num < 1 || track_num > 255) {
        ESP_LOGE(TAG, "Invalid track: %d", track_num);
        return false;
    }

    if (player_state == PLAYER_STATE_PLAYING || player_state == PLAYER_STATE_PAUSED) {
        audio_player_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    current_track = track_num;
    player_state = PLAYER_STATE_PLAYING;

    xTaskCreatePinnedToCore(audio_playback_task, "audio_play", 8192,
                            (void *)(uint32_t)track_num, 5, &audio_task_handle, 1);
    return true;
}

void audio_player_stop(void)
{
    player_state = PLAYER_STATE_STOPPED;
    if (audio_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(50));
        audio_task_handle = NULL;
    }
    audio_task_running = false;
    gpio_set_level(AUDIO_CODEC_PA_PIN, 0);
}

void audio_player_pause(void)
{
    if (player_state == PLAYER_STATE_PLAYING)
        player_state = PLAYER_STATE_PAUSED;
}

void audio_player_resume(void)
{
    if (player_state == PLAYER_STATE_PAUSED)
        player_state = PLAYER_STATE_PLAYING;
}

player_state_t audio_player_get_state(void)
{
    return player_state;
}

uint8_t audio_player_get_current_track(void)
{
    return current_track;
}