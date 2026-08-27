/*
 * ui_main.h - LVGL UI main interface
 */

#ifndef UI_MAIN_H
#define UI_MAIN_H

#include <stdint.h>
#include <stdbool.h>

/* UI states */
typedef enum {
    UI_STATE_WELCOME = 0,
    UI_STATE_MENU,        /* Main menu: Play / USB / Settings */
    UI_STATE_PLAYING,
    UI_STATE_STOPPED,
    UI_STATE_SETTINGS
} ui_state_t;

/* Menu items */
typedef enum {
    MENU_ITEM_PLAY = 0,
    MENU_ITEM_USB,
    MENU_ITEM_SETTINGS,
    MENU_ITEM_COUNT
} menu_item_t;

/* Callback types for UI events */
typedef void (*ui_btn_cb_t)(void);
typedef void (*ui_slider_cb_t)(uint8_t);
typedef void (*ui_usb_cb_t)(void);

/**
 * @brief Initialize the LVGL UI
 */
void ui_init(void);

/**
 * @brief Set UI event callbacks
 */
void ui_set_callbacks(ui_btn_cb_t prev_cb, ui_btn_cb_t play_cb, ui_btn_cb_t next_cb,
                      ui_slider_cb_t vol_cb, ui_slider_cb_t bright_cb,
                      ui_usb_cb_t usb_cb);

/**
 * @brief Set menu item execution callbacks (for touch click)
 */
void ui_set_menu_callbacks(void (*play)(void), void (*usb)(void), void (*settings)(void));

/**
 * @brief Show welcome screen
 */
void ui_show_welcome(void);

/**
 * @brief Show main menu
 */
void ui_show_menu(void);

/**
 * @brief Navigate menu (direction: -1=up, +1=down)
 */
void ui_menu_navigate(int8_t direction);

/**
 * @brief Confirm current menu selection, returns selected item
 */
menu_item_t ui_menu_confirm(void);

/**
 * @brief Get current menu selection
 */
menu_item_t ui_menu_get_selection(void);

/**
 * @brief Show playing screen with image
 */
void ui_show_playing(uint8_t track);

/**
 * @brief Show stopped screen
 */
void ui_show_stopped(void);

/**
 * @brief Show settings menu
 */
void ui_show_settings(void);

/**
 * @brief Hide settings menu / return to previous screen
 */
void ui_hide_settings(void);

/**
 * @brief Adjust settings via buttons (direction: -1=decrease, +1=increase)
 * Cycles focus between volume and brightness sliders.
 */
void ui_settings_adjust(int8_t direction);

/**
 * @brief Update volume display
 */
void ui_set_volume(uint8_t volume);

/**
 * @brief Update brightness display
 */
void ui_set_brightness(uint8_t brightness);

/**
 * @brief Update current track display
 */
void ui_set_track(uint8_t track);

/**
 * @brief Set image to display (RGB565 buffer)
 */
void ui_set_image(const uint16_t *pixels, uint16_t w, uint16_t h);

/**
 * @brief Set play button icon (play/pause)
 */
void ui_set_play_button_text(bool is_playing);

/**
 * @brief Get current UI state
 */
ui_state_t ui_get_state(void);

/* Task button callback type */
typedef void (*ui_task_btn_cb_t)(void);

/**
 * @brief Set task button callbacks (Task1, Task2)
 */
void ui_set_task_callbacks(ui_task_btn_cb_t task1_cb, ui_task_btn_cb_t task2_cb);

/**
 * @brief Update task status label text
 */
void ui_set_task_status(const char *text);

#endif // UI_MAIN_H