/*
 * audio_player.c - ES8311 audio playback via I2S + helix MP3 decoder
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "config.h"
#include "audio_player.h"
#include "fatfs_manager.h"

// For helix MP3 decoder
#include "mp3dec.h"

static const char *TAG = "AUDIO";

static player_state_t player_state = PLAYER_STATE_STOPPED;
static uint8_t current_track = 0;
static TaskHandle_t audio_task_handle = NULL;
static volatile bool audio_task_running = false;

// I2S handle
static i2s_chan_handle_t i2s_tx_handle = NULL;

// ES8311 register addresses
#define ES8311_RESET_REG     0x00
#define ES8311_CLOCK_MANAGER 0x01
#define ES8311_SDP_IN        0x0A
#define ES8311_SDP_OUT       0x0B
#define ES8311_ADC_CTRL      0x1B
#define ES8311_DAC_CTRL      0x1F
#define ES8311_DAC_VOL_L     0x22
#define ES8311_DAC_VOL_R     0x23
#define ES8311_ANALOG_MUX    0x2C
#define ES8311_LRCK_CFG      0x09
#define ES8311_HP_SEL        0x26
#define ES8311_HP_DRIVER_CTRL 0x27
#define ES8311_HP_CAL        0x28
#define ES8311_GPIO_CTRL     0x30
#define ES8311_GPIO_DS       0x31

// ES8311 I2C address (0x18 or 0x30, default 0x18)
#define ES8311_I2C_ADDR      0x18

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
    ESP_LOGI(TAG, "Initializing ES8311 codec");

    // Configure I2C master for codec control (using different I2C bus than slave)
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
    es8311_write_reg(ES8311_RESET_REG, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Select clock: MCLK = 16.384MHz, set proper dividers
    es8311_write_reg(ES8311_CLOCK_MANAGER, 0x00); // MCLK from GPIO, no divider
    vTaskDelay(pdMS_TO_TICKS(10));

    // Power up DAC
    es8311_write_reg(ES8311_DAC_CTRL, 0x24); // Enable DAC, soft ramp
    vTaskDelay(pdMS_TO_TICKS(10));

    // Set I2S format: 16-bit, I2S standard
    es8311_write_reg(ES8311_SDP_IN, 0x00);   // I2S format, 16-bit
    es8311_write_reg(ES8311_SDP_OUT, 0x00);  // I2S format, 16-bit

    // Set DAC volume (0dB)
    es8311_write_reg(ES8311_DAC_VOL_L, 0x00); // 0dB
    es8311_write_reg(ES8311_DAC_VOL_R, 0x00); // 0dB

    // Configure HP output
    es8311_write_reg(ES8311_HP_SEL, 0x05);    // HP output from DAC
    es8311_write_reg(ES8311_HP_DRIVER_CTRL, 0x1E); // Normal mode
    es8311_write_reg(ES8311_HP_CAL, 0x00);

    // Enable PA (power amplifier)
    gpio_set_direction(AUDIO_CODEC_PA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_CODEC_PA_PIN, 1);

    ESP_LOGI(TAG, "ES8311 initialized");
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
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_handle));
}

/* MP3 playback task */
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

    // Get file size
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Allocate buffer for MP3 data
    uint8_t *mp3_buf = malloc(fsize);
    if (!mp3_buf) {
        ESP_LOGE(TAG, "OOM for MP3 buffer (%ld bytes)", fsize);
        fclose(f);
        player_state = PLAYER_STATE_STOPPED;
        audio_task_running = false;
        audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    fread(mp3_buf, 1, fsize, f);
    fclose(f);

    // Initialize helix MP3 decoder
    HMP3Decoder decoder = MP3InitDecoder();
    if (!decoder) {
        ESP_LOGE(TAG, "MP3 decoder init failed");
        free(mp3_buf);
        player_state = PLAYER_STATE_STOPPED;
        audio_task_running = false;
        audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // MP3 decoder output buffer (max 1152 samples * 2 channels)
    int16_t pcm_buf[2304];
    uint8_t *input_ptr = mp3_buf;
    int input_bytes = fsize;
    int bytes_left = input_bytes;

    player_state = PLAYER_STATE_PLAYING;
    audio_task_running = true;

    while (bytes_left > 0 && player_state == PLAYER_STATE_PLAYING) {
        // Find sync word
        int offset = 0;
        while (offset < bytes_left - 1) {
            if (input_ptr[offset] == 0xFF && (input_ptr[offset + 1] & 0xE0) == 0xE0)
                break;
            offset++;
        }
        if (offset >= bytes_left) break;

        input_ptr += offset;
        bytes_left -= offset;

        if (bytes_left < 4) break;

        // Get frame size
        int frame_size = MP3GetNextFrameInfo(decoder, input_ptr);
        if (frame_size <= 0 || frame_size > bytes_left) break;

        // Decode frame
        int err = MP3Decode(decoder, &input_ptr, &bytes_left, pcm_buf, 0);
        if (err != ERR_MP3_NONE) {
            // Skip corrupted frame
            input_ptr += frame_size;
            bytes_left -= frame_size;
            continue;
        }

        // Get PCM output size
        int sample_rate = decoder->header.samplerate;
        int channels = decoder->header.nChans;
        int samples = decoder->outputSamps;

        if (samples > 0) {
            // Convert stereo to mono if needed, and write to I2S
            int16_t mono_buf[1152];
            int mono_samples = samples / channels;
            if (channels == 2) {
                for (int i = 0; i < mono_samples; i++) {
                    int32_t sum = (int32_t)pcm_buf[i * 2] + (int32_t)pcm_buf[i * 2 + 1];
                    mono_buf[i] = sum >> 1;
                }
            } else {
                memcpy(mono_buf, pcm_buf, samples * sizeof(int16_t));
            }

            size_t bytes_written = 0;
            i2s_channel_write(i2s_tx_handle, mono_buf, mono_samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        }

        input_ptr += frame_size;
        bytes_left -= frame_size;
    }

    ESP_LOGI(TAG, "Playback finished");
    MP3FreeDecoder(decoder);
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

    // Stop current playback
    if (player_state == PLAYER_STATE_PLAYING || player_state == PLAYER_STATE_PAUSED) {
        audio_player_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    current_track = track_num;
    player_state = PLAYER_STATE_PLAYING;

    // Create playback task
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
    if (player_state == PLAYER_STATE_PLAYING) {
        player_state = PLAYER_STATE_PAUSED;
    }
}

void audio_player_resume(void)
{
    if (player_state == PLAYER_STATE_PAUSED) {
        player_state = PLAYER_STATE_PLAYING;
    }
}

player_state_t audio_player_get_state(void)
{
    return player_state;
}

uint8_t audio_player_get_current_track(void)
{
    return current_track;
}