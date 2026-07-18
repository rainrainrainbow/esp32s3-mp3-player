set(COMPONENT_SRCS 
    "main.c" 
    "tft_driver.c" 
    "audio_player.c" 
    "i2c_slave.c" 
    "usb_msc.c" 
    "fatfs_manager.c"
    "image_decoder.c"
    "minimp3.c"
)
set(COMPONENT_ADD_INCLUDEDIRS ".")

# Use only built-in IDF components
set(COMPONENT_REQUIRES 
    driver 
    fatfs 
    spi_flash 
    wear_levelling 
    nvs_flash 
    i2s 
    esp_timer
    tinyusb
    tjpgd
)

register_component()