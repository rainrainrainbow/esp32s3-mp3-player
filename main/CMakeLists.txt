set(COMPONENT_SRCS 
    "main.c" 
    "tft_driver.c" 
    "audio_player.c" 
    "i2c_slave.c" 
    "usb_msc.c" 
    "fatfs_manager.c"
    "image_decoder.c"
)
set(COMPONENT_ADD_INCLUDEDIRS ".")

# Required IDF components
set(COMPONENT_REQUIRES 
    driver 
    fatfs 
    spi_flash 
    wear_levelling 
    nvs_flash 
    i2s 
    esp_timer
    tinyusb
    helix_mp3
    esp_iot_solution
)

register_component()