/*
 * ES8311 playback and diagnostics for ESP32-S3 / ESP-IDF v6.1.
 *
 * The codec register sequence below follows Espressif's ES8311 driver.
 * I2S always sends a 16-bit stereo frame; mono material is duplicated to
 * left/right so the codec receives a valid 32-BCLK frame.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "config.h"
#include "audio_player.h"
#include "minimp3.h"

static const char *TAG = "AUDIO";
#define ES8311_I2C_ADDR      0x18
#define AUDIO_DEFAULT_RATE   44100
#define TEST_SECONDS         3
#define TEST_AMPLITUDE       12000

static player_state_t player_state = PLAYER_STATE_STOPPED;
static uint8_t current_track = 0;
static TaskHandle_t audio_task_handle = NULL;
static i2s_chan_handle_t i2s_tx_handle = NULL;
static uint32_t i2s_sample_rate = AUDIO_DEFAULT_RATE;

static esp_err_t es8311_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    esp_err_t ret = i2c_master_write_to_device(I2C_NUM_1, ES8311_I2C_ADDR,
                                                data, sizeof(data),
                                                pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 write reg 0x%02X=0x%02X failed: %s",
                 reg, value, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t es8311_read_reg(uint8_t reg, uint8_t *value)
{
    esp_err_t ret = i2c_master_write_read_device(I2C_NUM_1, ES8311_I2C_ADDR,
                                                  &reg, 1, value, 1,
                                                  pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 read reg 0x%02X failed: %s",
                 reg, esp_err_to_name(ret));
    }
    return ret;
}

static bool codec_write(uint8_t reg, uint8_t value)
{
    return es8311_write_reg(reg, value) == ESP_OK;
}

static void es8311_dump_key_registers(void)
{
    static const uint8_t regs[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0D, 0x0E, 0x12, 0x13, 0x31, 0x32, 0x37
    };
    ESP_LOGI(TAG, "ES8311 key-register dump:");
    for (size_t i = 0; i < sizeof(regs); ++i) {
        uint8_t value = 0;
        if (es8311_read_reg(regs[i], &value) == ESP_OK) {
            ESP_LOGI(TAG, "  ES8311[0x%02X] = 0x%02X", regs[i], value);
        }
    }
}

static bool es8311_init(void)
{
    ESP_LOGI(TAG, "Initializing ES8311 at I2C address 0x%02X", ES8311_I2C_ADDR);

    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0,
    };
    esp_err_t ret = i2c_param_config(I2C_NUM_1, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = i2c_driver_install(I2C_NUM_1, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C install failed: %s", esp_err_to_name(ret));
        return false;
    }

    gpio_set_direction(AUDIO_CODEC_PA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_CODEC_PA_PIN, 0);

    /* Reset and power-on command from Espressif's official ES8311 driver. */
    if (!codec_write(0x00, 0x1F)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!codec_write(0x00, 0x00)) return false;
    if (!codec_write(0x00, 0x80)) return false;

    /*
     * Clock setup for MCLK = 256 * sample_rate.
     * With 16-bit stereo I2S, BCLK = 32 * sample_rate.
     */
    if (!codec_write(0x01, 0x3F)) return false; /* MCLK pin, enable clocks */
    if (!codec_write(0x02, 0x00)) return false; /* pre-divider/multiplier */
    if (!codec_write(0x03, 0x10)) return false; /* ADC OSR */
    if (!codec_write(0x04, 0x10)) return false; /* DAC OSR */
    if (!codec_write(0x05, 0x00)) return false; /* ADC/DAC dividers */
    if (!codec_write(0x06, 0x03)) return false; /* BCLK divider */
    if (!codec_write(0x07, 0x00)) return false; /* LRCK high divider */
    if (!codec_write(0x08, 0xFF)) return false; /* LRCK low divider: 256 */

    /* Slave, Philips I2S, 16-bit input/output. */
    if (!codec_write(0x09, 0x0C)) return false; /* serial data input (DAC) */
    if (!codec_write(0x0A, 0x0C)) return false; /* serial data output (ADC) */

    /* Analog and DAC power path from official driver. */
    if (!codec_write(0x0D, 0x01)) return false;
    if (!codec_write(0x0E, 0x02)) return false;
    if (!codec_write(0x12, 0x00)) return false; /* DAC powered up */
    if (!codec_write(0x13, 0x10)) return false; /* DAC -> output driver */
    if (!codec_write(0x1C, 0x6A)) return false;
    if (!codec_write(0x37, 0x08)) return false; /* bypass DAC EQ */

    /* Unmute and set a conservative diagnostic volume (~75%). */
    if (!codec_write(0x31, 0x00)) return false;
    if (!codec_write(0x32, 0xBF)) return false;

    ESP_LOGI(TAG, "ES8311 initialized with official 16-bit I2S sequence");
    es8311_dump_key_registers();
    return true;
}

static bool i2s_set_sample_rate(uint32_t rate)
{
    if (!i2s_tx_handle || rate < 8000 || rate > 96000) {
        ESP_LOGE(TAG, "Unsupported I2S sample rate: %lu", (unsigned long)rate);
        return false;
    }
    if (rate == i2s_sample_rate) return true;

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    esp_err_t ret = i2s_channel_disable(i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S disable failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = i2s_channel_reconfig_std_clock(i2s_tx_handle, &clk_cfg);
    if (ret == ESP_OK) ret = i2s_channel_enable(i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S rate change to %lu failed: %s",
                 (unsigned long)rate, esp_err_to_name(ret));
        return false;
    }
    i2s_sample_rate = rate;
    ESP_LOGI(TAG, "I2S sample rate=%lu Hz, MCLK=%lu Hz",
             (unsigned long)rate, (unsigned long)(rate * 256U));
    return true;
}

static bool i2s_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S: Philips, 16-bit stereo, MCLK=256*Fs");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                            I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &i2s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return false;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_DEFAULT_RATE);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    const i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
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
    ret = i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg);
    if (ret == ESP_OK) ret = i2s_channel_enable(i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(ret));
        return false;
    }
    i2s_sample_rate = AUDIO_DEFAULT_RATE;
    ESP_LOGI(TAG, "I2S ready: Fs=44100, MCLK=11289600, BCLK=1411200");
    return true;
}

static esp_err_t write_stereo(const int16_t *data, size_t frames)
{
    size_t written = 0;
    size_t bytes = frames * 2U * sizeof(int16_t);
    esp_err_t ret = i2s_channel_write(i2s_tx_handle, data, bytes, &written,
                                      portMAX_DELAY);
    if (ret != ESP_OK || written != bytes) {
        ESP_LOGE(TAG, "I2S write failed: ret=%s, requested=%u, written=%u",
                 esp_err_to_name(ret), (unsigned)bytes, (unsigned)written);
        return ret != ESP_OK ? ret : ESP_FAIL;
    }
    return ESP_OK;
}

static void audio_output_begin(void)
{
    int16_t silence[128 * 2] = {0};
    write_stereo(silence, 128);
    gpio_set_level(AUDIO_CODEC_PA_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
}

static void audio_output_end(void)
{
    int16_t silence[128 * 2] = {0};
    write_stereo(silence, 128);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(AUDIO_CODEC_PA_PIN, 0);
}

static void playback_cleanup(uint8_t *mp3, int16_t *pcm, int16_t *stereo)
{
    free(mp3);
    free(pcm);
    free(stereo);
    audio_output_end();
    player_state = PLAYER_STATE_STOPPED;
    audio_task_handle = NULL;
    vTaskDelete(NULL);
}

static void audio_playback_task(void *param)
{
    uint8_t track = (uint8_t)(uintptr_t)param;
    char path[96];
    snprintf(path, sizeof(path), "%s/%u.mp3", MUSIC_DIR, track);

    struct stat st;
    errno = 0;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "stat('%s') failed: errno=%d (%s)",
                 path, errno, strerror(errno));
    } else {
        ESP_LOGI(TAG, "MP3 stat: path=%s size=%ld", path, (long)st.st_size);
    }

    errno = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen('%s') failed: errno=%d (%s)",
                 path, errno, strerror(errno));
        player_state = PLAYER_STATE_STOPPED;
        audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "fseek failed: errno=%d (%s)", errno, strerror(errno));
        fclose(f);
        player_state = PLAYER_STATE_STOPPED;
        audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid MP3 size: %ld", file_size);
        fclose(f);
        player_state = PLAYER_STATE_STOPPED;
        audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t *mp3 = heap_caps_malloc((size_t)file_size,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mp3) mp3 = malloc((size_t)file_size);
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *stereo = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mp3 || !pcm || !stereo) {
        ESP_LOGE(TAG, "Audio buffer allocation failed (file=%ld)", file_size);
        fclose(f);
        free(mp3); free(pcm); free(stereo);
        player_state = PLAYER_STATE_STOPPED;
        audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    size_t bytes_read = fread(mp3, 1, (size_t)file_size, f);
    fclose(f);
    if (bytes_read != (size_t)file_size) {
        ESP_LOGE(TAG, "Short MP3 read: expected=%ld actual=%u errno=%d (%s)",
                 file_size, (unsigned)bytes_read, errno, strerror(errno));
        playback_cleanup(mp3, pcm, stereo);
        return;
    }
    ESP_LOGI(TAG, "MP3 loaded: %u bytes", (unsigned)bytes_read);

    mp3dec_t dec;
    mp3dec_init(&dec);
    uint8_t *input = mp3;
    int bytes_left = (int)bytes_read;
    bool output_started = false;
    int decoded_frames = 0;
    int invalid_bytes = 0;

    while (bytes_left > 0 && player_state == PLAYER_STATE_PLAYING) {
        mp3dec_frame_info_t info = {0};
        int samples = mp3dec_decode_frame(&dec, input, bytes_left, pcm, &info);
        if (info.frame_bytes <= 0) {
            ++input;
            --bytes_left;
            ++invalid_bytes;
            continue;
        }
        input += info.frame_bytes;
        bytes_left -= info.frame_bytes;
        if (samples <= 0) continue;

        if (!output_started) {
            ESP_LOGI(TAG,
                     "First MP3 frame: Hz=%d channels=%d samples/ch=%d bitrate=%dk layer=%d skipped=%d",
                     info.hz, info.channels, samples, info.bitrate_kbps,
                     info.layer, invalid_bytes);
            if (!i2s_set_sample_rate((uint32_t)info.hz)) break;
            audio_output_begin();
            output_started = true;
        }

        /* minimp3 returns sample count PER CHANNEL. */
        if (info.channels == 2) {
            if (write_stereo(pcm, (size_t)samples) != ESP_OK) break;
        } else if (info.channels == 1) {
            for (int i = 0; i < samples; ++i) {
                stereo[i * 2] = pcm[i];
                stereo[i * 2 + 1] = pcm[i];
            }
            if (write_stereo(stereo, (size_t)samples) != ESP_OK) break;
        } else {
            ESP_LOGE(TAG, "Invalid MP3 channel count: %d", info.channels);
            break;
        }
        ++decoded_frames;
    }

    if (!output_started) {
        ESP_LOGE(TAG, "No valid MP3 audio frame decoded; skipped=%d bytes", invalid_bytes);
    } else {
        ESP_LOGI(TAG, "Playback ended: decoded_frames=%d remaining=%d",
                 decoded_frames, bytes_left);
    }
    playback_cleanup(mp3, pcm, stereo);
}

void audio_player_play_test_tone(void)
{
    if (!i2s_tx_handle) {
        ESP_LOGE(TAG, "Diagnostic melody unavailable: I2S not initialized");
        return;
    }
    if (audio_task_handle) {
        ESP_LOGW(TAG, "Diagnostic melody skipped: MP3 task is active");
        return;
    }

    ESP_LOGI(TAG, "DIAG AUDIO START: 3-second A4/C#5/E5 melody");
    if (!i2s_set_sample_rate(AUDIO_DEFAULT_RATE)) return;

    int16_t *buf = heap_caps_malloc(256 * 2 * sizeof(int16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        ESP_LOGE(TAG, "Diagnostic buffer allocation failed");
        return;
    }

    static const double notes[3] = {440.0, 554.365, 659.255};
    const int total_frames = AUDIO_DEFAULT_RATE * TEST_SECONDS;
    double phase = 0.0;
    audio_output_begin();
    int produced = 0;
    while (produced < total_frames) {
        int count = total_frames - produced;
        if (count > 256) count = 256;
        for (int i = 0; i < count; ++i) {
            int absolute = produced + i;
            int note = absolute / AUDIO_DEFAULT_RATE;
            double freq = notes[note > 2 ? 2 : note];
            phase += 2.0 * M_PI * freq / AUDIO_DEFAULT_RATE;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            int16_t sample = (int16_t)(TEST_AMPLITUDE * sin(phase));
            buf[i * 2] = sample;
            buf[i * 2 + 1] = sample;
        }
        if (write_stereo(buf, (size_t)count) != ESP_OK) break;
        produced += count;
    }
    audio_output_end();
    free(buf);
    ESP_LOGI(TAG, "DIAG AUDIO END: frames=%d", produced);
}

void audio_player_init(void)
{
    ESP_LOGI(TAG, "Audio player init");
    bool i2s_ok = i2s_init();
    bool codec_ok = i2s_ok && es8311_init();
    ESP_LOGI(TAG, "Audio init result: I2S=%s ES8311=%s",
             i2s_ok ? "OK" : "FAIL", codec_ok ? "OK" : "FAIL");
}

bool audio_player_play_track(uint8_t track_num)
{
    if (track_num < 1) return false;
    if (audio_task_handle) {
        player_state = PLAYER_STATE_STOPPED;
        for (int i = 0; i < 100 && audio_task_handle; ++i) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (audio_task_handle) {
            ESP_LOGE(TAG, "Previous audio task did not stop");
            return false;
        }
    }

    current_track = track_num;
    player_state = PLAYER_STATE_PLAYING;
    BaseType_t ok = xTaskCreatePinnedToCore(audio_playback_task, "audio_play",
                                            16384,
                                            (void *)(uintptr_t)track_num,
                                            5, &audio_task_handle, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
        player_state = PLAYER_STATE_STOPPED;
        audio_task_handle = NULL;
        return false;
    }
    return true;
}

void audio_player_stop(void)
{
    player_state = PLAYER_STATE_STOPPED;
    gpio_set_level(AUDIO_CODEC_PA_PIN, 0);
}

void audio_player_pause(void)
{
    if (player_state == PLAYER_STATE_PLAYING) player_state = PLAYER_STATE_PAUSED;
}

void audio_player_resume(void)
{
    if (player_state == PLAYER_STATE_PAUSED) player_state = PLAYER_STATE_PLAYING;
}

player_state_t audio_player_get_state(void) { return player_state; }
uint8_t audio_player_get_current_track(void) { return current_track; }
