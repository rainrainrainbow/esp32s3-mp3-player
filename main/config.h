#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/uart.h>

/* ========== Audio Configuration ========== */
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// I2S for ES8311
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_45
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_41   // LRCK
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_39   // BCLK
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_40   // I2S_SD_IN (from codec)
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_42   // I2S_SD_OUT (to codec)

// ES8311 I2C control
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_46
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_4
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_5

/* ========== Display Configuration ========== */
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY true

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

// SPI LCD - ST7789 or compatible
#define DISPLAY_DC_GPIO     GPIO_NUM_1
#define DISPLAY_CS_GPIO     GPIO_NUM_2
#define DISPLAY_CLK_GPIO    GPIO_NUM_21
#define DISPLAY_MOSI_GPIO   GPIO_NUM_47
#define DISPLAY_RST_GPIO    GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_14
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE        0
#define DISPLAY_SPI_HOST        SPI2_HOST

/* ========== I2C Slave Configuration ========== */
#define I2C_SLAVE_SDA        GPIO_NUM_38
#define I2C_SLAVE_SCL        GPIO_NUM_48
#define I2C_SLAVE_ADDR       0x52
#define I2C_SLAVE_PORT       I2C_NUM_0

/* I2C Slave Registers */
#define REG_PLAY_TRACK       0x01  // Write: select track 1-255
#define REG_PLAY_STATUS      0x02  // Read: 0=stopped, 1=playing, 2=paused

/* ========== Storage Configuration ========== */
#define STORAGE_PARTITION_LABEL  "storage"
#define STORAGE_MOUNT_POINT      "/spiflash"
#define STORAGE_MAX_FILES        32

/* ========== Playback Paths ========== */
#define MUSIC_DIR   "/spiflash/music"
#define IMAGE_DIR   "/spiflash/images"

/* ========== Pin Mappings ========== */
#define LEFT_BUTTON_GPIO  GPIO_NUM_0
#define RIGHT_BUTTON_GPIO GPIO_NUM_43

// UART for voice chip
#define UART0_RXD     GPIO_NUM_44
#define UART0_TXD     GPIO_NUM_NC
#define UART0_PORT_NUM    UART_NUM_0
#define UART0_BAUD_RATE   (115200)

/* ========== Camera (unused but keep for reference) ========== */
#define HIWONDER_CAMERA_XCLK      GPIO_NUM_15
#define HIWONDER_CAMERA_PCLK      GPIO_NUM_13
#define HIWONDER_CAMERA_VSYNC     GPIO_NUM_6
#define HIWONDER_CAMERA_HSYNC     GPIO_NUM_7
#define HIWONDER_CAMERA_D0        GPIO_NUM_11
#define HIWONDER_CAMERA_D1        GPIO_NUM_9
#define HIWONDER_CAMERA_D2        GPIO_NUM_8
#define HIWONDER_CAMERA_D3        GPIO_NUM_10
#define HIWONDER_CAMERA_D4        GPIO_NUM_12
#define HIWONDER_CAMERA_D5        GPIO_NUM_18
#define HIWONDER_CAMERA_D6        GPIO_NUM_17
#define HIWONDER_CAMERA_D7        GPIO_NUM_16
#define HIWONDER_CAMERA_PWDN      GPIO_NUM_NC
#define HIWONDER_CAMERA_RESET     GPIO_NUM_NC
#define HIWONDER_CAMERA_XCLK_FREQ (8000000)
#define HIWONDER_LEDC_TIMER       (LEDC_TIMER_0)
#define HIWONDER_LEDC_CHANNEL     (LEDC_CHANNEL_0)
#define HIWONDER_CAMERA_SIOD      GPIO_NUM_NC
#define HIWONDER_CAMERA_SIOC      GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_