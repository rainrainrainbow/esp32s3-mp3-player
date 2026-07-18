# ESP32-S3 MP3 Player with I2C Slave Control

## Hardware
- **MCU**: ESP32-S3 N16R8 (16MB flash, 8MB OPI PSRAM)
- **Audio**: ES8311 codec via I2S (GPIO 45/41/39/40/42)
- **Display**: 240x320 TFT (SPI: GPIO 1/2/21/47/14)
- **I2C Slave**: 0x52 (GPIO 38/48) for external MCU control
- **USB**: USB Mass Storage for file transfers (no SD card needed)

## Features
- 🎵 **MP3 Playback**: Read MP3 from SPI flash FATFS, decoded with Helix MP3, output via ES8311
- 🖼️ **Image Slideshow**: Display JPEG/BMP from flash while playing (5s rotation)
- 🔌 **I2C Slave Control**: External MCU sends commands at address 0x52
- 💾 **USB Mass Storage**: Plug into PC to copy MP3/images to flash via USB drive
- 📁 **No SD Card**: All storage on SPI flash with wear levelling

## I2C Protocol (Address: 0x52)
| Register | Access | Description |
|----------|--------|-------------|
| 0x01     | Write  | Play track number (1-255) |
| 0x02     | Read   | Play status: 0=stopped, 1=playing, 2=paused |

### Protocol Example
```
I2C Master → 0x52: [0x01, 0x05]  → Play track 5
I2C Master → 0x52: [0x02]        → Read status
I2C Slave  → Master: 0x01       → Currently playing
```

## File Structure (on USB drive)
```
/music/001.mp3    - Track 1
/music/002.mp3    - Track 2
...
/images/          - Put JPEG/BMP images here (auto slideshow during playback)
```

## Flashing
```bash
# Flash individual parts
esptool.py --chip esp32s3 -p COM_PORT write_flash \
  --flash_mode qio --flash_freq 80m --flash_size 16MB \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 esp32s3_mp3_player.bin

# Or use merged binary
esptool.py --chip esp32s3 -p COM_PORT write_flash 0x0 firmware_merged.bin
```

## Building Locally
```bash
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

## Pin Mapping
| Function | GPIO |
|----------|------|
| I2S MCLK | 45 |
| I2S LRCK | 41 |
| I2S BCLK | 39 |
| I2S DIN | 40 |
| I2S DOUT | 42 |
| ES8311 SDA | 4 |
| ES8311 SCL | 5 |
| PA Enable | 46 |
| TFT DC | 1 |
| TFT CS | 2 |
| TFT SCK | 21 |
| TFT MOSI | 47 |
| TFT BL | 14 |
| I2C Slave SDA | 38 |
| I2C Slave SCL | 48 |