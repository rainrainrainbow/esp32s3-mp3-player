/**
 * @file lv_conf.h
 * Configuration file for LVGL v8.3
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1

/*====================
   MEMORY SETTINGS
 *====================*/
#define LV_MEM_CUSTOM 1
#define LV_MEMCPY_MEMSET_STD 1

/*====================
   HAL SETTINGS
 *====================*/
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (esp_timer_get_time() / 1000)

/*====================
   FONT USAGE
 *====================*/
#define LV_FONT_MONTSERRAT_48 1

/*====================
   FS SETTINGS
 *====================*/
#define LV_USE_FS_POSIX 1
#define LV_FS_POSIX_LETTER 'A'

/*====================
   OTHERS
 *====================*/
#define LV_LABEL_TEXT_SELECTION 1

#endif /* LV_CONF_H */
