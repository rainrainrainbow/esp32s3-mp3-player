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
    UI_STATE_PLAYING,
    UI_STATE_STOPPED,
    UI_STATE_SETTINGS,
    UI_STATE_CONTROL
} ui_state_t;

/* Callback types for UI events */
typedef void (*ui_btn_cb_t)(void);
typedef void (*ui_slider_cb_t)(uint8_t);

/**
 * @brief Initialize the LVGL UI
 */
void ui_init(void);

/**
 * @brief Set UI event callbacks
 */
void ui_set_callbacks(ui_btn_cb_t prev_cb, ui_btn_cb_t play_cb, ui_btn_cb_t next_cb,
                      ui_slider_cb_t vol_cb, ui_slider_cb_t bright_cb,
                      ui_btn_cb_t task1_cb, ui_btn_cb_t task2_cb);

/**
 * @brief Show welcome screen
 */
void ui_show_welcome(void);

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
 * @brief Show car control screen (task buttons)
 */
void ui_show_control(void);

/**
 * @brief Hide settings menu
 */
void ui_hide_settings(void);

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

#endif // UI_MAIN_H