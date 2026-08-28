/*
 * ui_main.c - LVGL UI for Smart Car Controller
 * Simplified interface: Task1/Task2 buttons + Settings
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "config.h"
#include "ui_main.h"

static const char *TAG = "UI";

static ui_state_t current_state = UI_STATE_WELCOME;

/* UI object pointers */
static lv_obj_t *scr_welcome = NULL;
static lv_obj_t *scr_menu = NULL;
static lv_obj_t *scr_playing = NULL;    /* Music playing screen */
static lv_obj_t *scr_stopped = NULL;    /* Music stopped screen */
static lv_obj_t *scr_task_running = NULL;
static lv_obj_t *settings_panel = NULL;

/* Welcome screen objects */
static lv_obj_t *welcome_bar = NULL;

/* Menu objects */
static lv_obj_t *btn_play = NULL;       /* Play/Pause button in menu */
static lv_obj_t *btn_prev = NULL;       /* Previous track */
static lv_obj_t *btn_next = NULL;       /* Next track */
static lv_obj_t *btn_task1 = NULL;
static lv_obj_t *btn_task2 = NULL;
static lv_obj_t *btn_usb = NULL;
static lv_obj_t *btn_settings = NULL;
static lv_obj_t *status_label = NULL;

/* Playing screen objects */
static lv_obj_t *play_img_obj = NULL;   /* Image display during playback */
static lv_obj_t *track_label = NULL;    /* Track number label */
static lv_obj_t *play_status_label = NULL; /* Playing status */

/* Stopped screen objects */
static lv_obj_t *stopped_label = NULL;

/* Task running screen objects */
static lv_obj_t *task_img_obj = NULL;
static lv_obj_t *task_status_label = NULL;

/* Settings objects */
static lv_obj_t *vol_slider = NULL;
static lv_obj_t *bright_slider = NULL;
static lv_obj_t *vol_label = NULL;
static lv_obj_t *bright_label = NULL;

/* Image buffers */
static lv_color_t *play_img_buf = NULL;      /* Buffer for slideshow images */
static lv_img_dsc_t play_img_dsc = {0};
static lv_color_t *task_img_buf = NULL;      /* Buffer for task startup image */
static lv_img_dsc_t task_img_dsc = {0};

/* Callbacks */
static void (*on_prev_cb)(void) = NULL;
static void (*on_play_cb)(void) = NULL;
static void (*on_next_cb)(void) = NULL;
static void (*on_task1_cb)(void) = NULL;
static void (*on_task2_cb)(void) = NULL;
static void (*on_usb_cb)(void) = NULL;
static void (*on_vol_change_cb)(uint8_t) = NULL;
static void (*on_bright_change_cb)(uint8_t) = NULL;

/* Previous state for settings return */
static ui_state_t prev_state = UI_STATE_MENU;

/* Settings focus: 0=volume, 1=brightness */
static uint8_t settings_focus = 0;

/* ========== Color Scheme ========== */
#define COLOR_BG       lv_color_hex(0x1A1A2E)
#define COLOR_SURFACE  lv_color_hex(0x16213E)
#define COLOR_PRIMARY  lv_color_hex(0x0F3460)
#define COLOR_ACCENT   lv_color_hex(0xE94560)
#define COLOR_TEXT     lv_color_hex(0xEAEAEA)
#define COLOR_MUTED    lv_color_hex(0x888888)
#define COLOR_SELECTED lv_color_hex(0x2D5F8A)

/* ========== Callback Setters ========== */
void ui_set_callbacks(void (*prev)(void), void (*play)(void), void (*next)(void),
                      void (*vol)(uint8_t), void (*bright)(uint8_t),
                      void (*usb)(void))
{
    on_prev_cb = prev;
    on_play_cb = play;
    on_next_cb = next;
    on_vol_change_cb = vol;
    on_bright_change_cb = bright;
    on_usb_cb = usb;
}

void ui_set_task_callbacks(ui_task_btn_cb_t task1_cb, ui_task_btn_cb_t task2_cb)
{
    on_task1_cb = task1_cb;
    on_task2_cb = task2_cb;
}

/* ========== Button Handlers ========== */
static void btn_prev_handler(lv_event_t *e)
{
    (void)e;
    if (on_prev_cb) on_prev_cb();
}

static void btn_play_handler(lv_event_t *e)
{
    (void)e;
    if (on_play_cb) on_play_cb();
}

static void btn_next_handler(lv_event_t *e)
{
    (void)e;
    if (on_next_cb) on_next_cb();
}

static void btn_task1_handler(lv_event_t *e)
{
    (void)e;
    if (on_task1_cb) {
        on_task1_cb();
        ui_show_task_running();
        if (status_label) {
            lv_label_set_text(status_label, "Task 1 Running");
        }
    }
}

static void btn_task2_handler(lv_event_t *e)
{
    (void)e;
    if (on_task2_cb) {
        on_task2_cb();
        ui_show_task_running();
        if (status_label) {
            lv_label_set_text(status_label, "Task 2 Running");
        }
    }
}

static void btn_usb_handler(lv_event_t *e)
{
    (void)e;
    if (on_usb_cb) {
        on_usb_cb();
    }
}

static void btn_settings_handler(lv_event_t *e)
{
    (void)e;
    ui_show_settings();
}

static void close_settings_handler(lv_event_t *e)
{
    ui_hide_settings();
}

static void vol_slider_handler(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t val = (uint8_t)lv_slider_get_value(slider);
    if (on_vol_change_cb) on_vol_change_cb(val);
}

static void bright_slider_handler(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t val = (uint8_t)lv_slider_get_value(slider);
    if (on_bright_change_cb) on_bright_change_cb(val);
}

/* ========== Create Welcome Screen ========== */
static void create_welcome_screen(void)
{
    scr_welcome = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_welcome, COLOR_BG, 0);

    lv_obj_t *welcome_label = lv_label_create(scr_welcome);
    lv_label_set_text(welcome_label, "Smart Car\nController");
    lv_obj_set_style_text_color(welcome_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_align(welcome_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(welcome_label, LV_ALIGN_CENTER, 0, -30);

    welcome_bar = lv_bar_create(scr_welcome);
    lv_obj_set_size(welcome_bar, 200, 6);
    lv_obj_align(welcome_bar, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(welcome_bar, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_color(welcome_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_bar_set_value(welcome_bar, 0, LV_ANIM_OFF);

    lv_obj_t *loading = lv_label_create(scr_welcome);
    lv_label_set_text(loading, "Initializing...");
    lv_obj_set_style_text_color(loading, COLOR_MUTED, 0);
    lv_obj_align(loading, LV_ALIGN_CENTER, 0, 50);
}

/* ========== Create Main Menu Screen ========== */
static void create_menu_screen(void)
{
    scr_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_menu, COLOR_BG, 0);

    /* Task Button 1 */
    btn_task1 = lv_btn_create(scr_menu);
    lv_obj_set_size(btn_task1, DISPLAY_WIDTH - 80, 50);
    lv_obj_align(btn_task1, LV_ALIGN_TOP_MID, 0, 15);
    lv_obj_set_style_bg_color(btn_task1, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_color(btn_task1, COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn_task1, 10, 0);
    lv_obj_add_event_cb(btn_task1, btn_task1_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_task1 = lv_label_create(btn_task1);
    lv_label_set_text(lbl_task1, LV_SYMBOL_PLAY " Task 1");
    lv_obj_center(lbl_task1);

    /* Task Button 2 */
    btn_task2 = lv_btn_create(scr_menu);
    lv_obj_set_size(btn_task2, DISPLAY_WIDTH - 80, 50);
    lv_obj_align(btn_task2, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_style_bg_color(btn_task2, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_color(btn_task2, COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn_task2, 10, 0);
    lv_obj_add_event_cb(btn_task2, btn_task2_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_task2 = lv_label_create(btn_task2);
    lv_label_set_text(lbl_task2, LV_SYMBOL_PLAY " Task 2");
    lv_obj_center(lbl_task2);

    /* USB Mode Button */
    btn_usb = lv_btn_create(scr_menu);
    lv_obj_set_size(btn_usb, DISPLAY_WIDTH - 80, 45);
    lv_obj_align(btn_usb, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_style_bg_color(btn_usb, lv_color_hex(0x2D5F8A), 0);
    lv_obj_set_style_radius(btn_usb, 8, 0);
    lv_obj_add_event_cb(btn_usb, btn_usb_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_usb = lv_label_create(btn_usb);
    lv_label_set_text(lbl_usb, LV_SYMBOL_USB " USB Mode");
    lv_obj_center(lbl_usb);

    /* Settings Button */
    btn_settings = lv_btn_create(scr_menu);
    lv_obj_set_size(btn_settings, DISPLAY_WIDTH - 80, 45);
    lv_obj_align(btn_settings, LV_ALIGN_TOP_MID, 0, 185);
    lv_obj_set_style_bg_color(btn_settings, lv_color_hex(0x2D5F8A), 0);
    lv_obj_set_style_radius(btn_settings, 8, 0);
    lv_obj_add_event_cb(btn_settings, btn_settings_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_settings = lv_label_create(btn_settings);
    lv_label_set_text(lbl_settings, LV_SYMBOL_SETTINGS " Settings");
    lv_obj_center(lbl_settings);

    /* Status label (bottom) */
    status_label = lv_label_create(scr_menu);
    lv_label_set_text(status_label, "Ready");
    lv_obj_set_style_text_color(status_label, COLOR_MUTED, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID , 0, -4);
}
/* ========== Create Playing Screen ========== */
static void create_playing_screen(void)
{
    scr_playing = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_playing, COLOR_BG, 0);

    /* Full-screen image display */
    play_img_obj = lv_img_create(scr_playing);
    lv_obj_set_size(play_img_obj, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_align(play_img_obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(play_img_obj, lv_color_hex(0x000000), 0);

    /* Track number label */
    track_label = lv_label_create(scr_playing);
    lv_label_set_text(track_label, "Track: 1");
    lv_obj_set_style_text_color(track_label, COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(track_label, lv_color_hex(0x80000000), 0);
    lv_obj_set_style_pad_all(track_label, 6, 0);
    lv_obj_align(track_label, LV_ALIGN_TOP_LEFT, 10, 10);

    /* Playing status */
    play_status_label = lv_label_create(scr_playing);
    lv_label_set_text(play_status_label, LV_SYMBOL_PLAY " Playing");
    lv_obj_set_style_text_color(play_status_label, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(play_status_label, lv_color_hex(0x80000000), 0);
    lv_obj_set_style_pad_all(play_status_label, 6, 0);
    lv_obj_align(play_status_label, LV_ALIGN_TOP_RIGHT, -10, 10);
}

/* ========== Create Stopped Screen ========== */
static void create_stopped_screen(void)
{
    scr_stopped = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_stopped, COLOR_BG, 0);

    stopped_label = lv_label_create(scr_stopped);
    lv_label_set_text(stopped_label, LV_SYMBOL_PAUSE " Stopped");
    lv_obj_set_style_text_color(stopped_label, COLOR_TEXT, 0);
    lv_obj_align(stopped_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = lv_label_create(scr_stopped);
    lv_label_set_text(hint, "Press Play to resume");
    lv_obj_set_style_text_color(hint, COLOR_MUTED, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

/* ========== Create Task Running Screen ========== */
static void create_task_running_screen(void)
{
    scr_task_running = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_task_running, COLOR_BG, 0);

    /* Full-screen image for startup image (0.png) */
    task_img_obj = lv_img_create(scr_task_running);
    lv_obj_set_size(task_img_obj, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_align(task_img_obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(task_img_obj, lv_color_hex(0x000000), 0);

    /* Status overlay */
    task_status_label = lv_label_create(scr_task_running);
    lv_label_set_text(task_status_label, "Task Running...");
    lv_obj_set_style_text_color(task_status_label, COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(task_status_label, lv_color_hex(0x80000000), 0);
    lv_obj_set_style_pad_all(task_status_label, 8, 0);
    lv_obj_align(task_status_label, LV_ALIGN_TOP_LEFT, 10, 10);

    /* Bottom hint */
    lv_obj_t *hint = lv_label_create(scr_task_running);
    lv_label_set_text(hint, "Press GPIO0 to exit");
    lv_obj_set_style_text_color(hint, COLOR_MUTED, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/* ========== Create Settings Panel ========== */
static void create_settings_panel(void)
{
    settings_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(settings_panel, DISPLAY_WIDTH - 20, 180);
    lv_obj_align(settings_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(settings_panel, COLOR_SURFACE, 0);
    lv_obj_set_style_radius(settings_panel, 10, 0);
    lv_obj_set_style_border_width(settings_panel, 2, 0);
    lv_obj_set_style_border_color(settings_panel, COLOR_ACCENT, 0);
    lv_obj_set_style_shadow_width(settings_panel, 20, 0);
    lv_obj_set_style_shadow_color(settings_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(settings_panel, 5, 0);

    lv_obj_t *title = lv_label_create(settings_panel);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *vol_title = lv_label_create(settings_panel);
    lv_label_set_text(vol_title, "Volume");
    lv_obj_set_style_text_color(vol_title, COLOR_MUTED, 0);
    lv_obj_align(vol_title, LV_ALIGN_TOP_LEFT, 15, 35);

    vol_slider = lv_slider_create(settings_panel);
    lv_obj_set_size(vol_slider, 180, 8);
    lv_obj_align(vol_slider, LV_ALIGN_TOP_LEFT, 15, 55);
    lv_slider_set_range(vol_slider, 0, 100);
    lv_slider_set_value(vol_slider, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vol_slider, COLOR_BG, 0);
    lv_obj_set_style_bg_color(vol_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_event_cb(vol_slider, vol_slider_handler, LV_EVENT_VALUE_CHANGED, NULL);

    vol_label = lv_label_create(settings_panel);
    lv_label_set_text(vol_label, "50%");
    lv_obj_set_style_text_color(vol_label, COLOR_TEXT, 0);
    lv_obj_align(vol_label, LV_ALIGN_TOP_LEFT, 200, 55);

    lv_obj_t *bright_title = lv_label_create(settings_panel);
    lv_label_set_text(bright_title, "Brightness");
    lv_obj_set_style_text_color(bright_title, COLOR_MUTED, 0);
    lv_obj_align(bright_title, LV_ALIGN_TOP_LEFT, 15, 85);

    bright_slider = lv_slider_create(settings_panel);
    lv_obj_set_size(bright_slider, 180, 8);
    lv_obj_align(bright_slider, LV_ALIGN_TOP_LEFT, 15, 105);
    lv_slider_set_range(bright_slider, 0, 100);
    lv_slider_set_value(bright_slider, 75, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bright_slider, COLOR_BG, 0);
    lv_obj_set_style_bg_color(bright_slider, COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_add_event_cb(bright_slider, bright_slider_handler, LV_EVENT_VALUE_CHANGED, NULL);

    bright_label = lv_label_create(settings_panel);
    lv_label_set_text(bright_label, "75%");
    lv_obj_set_style_text_color(bright_label, COLOR_TEXT, 0);
    lv_obj_align(bright_label, LV_ALIGN_TOP_LEFT, 200, 105);

    lv_obj_t *close_btn = lv_btn_create(settings_panel);
    lv_obj_set_size(close_btn, 80, 28);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(close_btn, COLOR_ACCENT, 0);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, close_settings_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ========== Public API ========== */

void ui_init(void)
{
    ESP_LOGI(TAG, "Creating UI screens...");

    create_welcome_screen();
    create_menu_screen();
    create_playing_screen();
    create_stopped_screen();
    create_task_running_screen();
    create_settings_panel();

    lv_scr_load(scr_welcome);
    current_state = UI_STATE_WELCOME;

    ESP_LOGI(TAG, "UI initialized");
}

void ui_show_welcome(void)
{
    if (scr_welcome) {
        lv_scr_load(scr_welcome);
        current_state = UI_STATE_WELCOME;
    }
}

void ui_show_menu(void)
{
    if (scr_menu) {
        lv_scr_load(scr_menu);
        current_state = UI_STATE_MENU;
        if (status_label) {
            lv_label_set_text(status_label, "Ready");
        }
    }
}

void ui_show_task_running(void)
{
    if (scr_task_running) {
        lv_scr_load(scr_task_running);
        current_state = UI_STATE_TASK_RUNNING;
        if (task_status_label) {
            lv_label_set_text(task_status_label, "Task Running...");
        }
    }
}

void ui_return_to_menu(void)
{
    ui_show_menu();
    if (status_label) {
        lv_label_set_text(status_label, "Task Completed");
    }
}

void ui_show_settings(void)
{
    if (settings_panel) {
        prev_state = current_state;
        lv_obj_clear_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(settings_panel);
        current_state = UI_STATE_SETTINGS;
        settings_focus = 0;
        if (vol_slider) {
            lv_obj_set_style_outline_width(vol_slider, 2, 0);
            lv_obj_set_style_outline_color(vol_slider, COLOR_ACCENT, 0);
        }
        if (bright_slider) {
            lv_obj_set_style_outline_width(bright_slider, 0, 0);
        }
    }
}

void ui_hide_settings(void)
{
    if (settings_panel) {
        lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
        current_state = prev_state;
        settings_focus = 0;
    }
}

void ui_settings_adjust(int8_t direction)
{
    if (!settings_panel || lv_obj_has_flag(settings_panel, LV_OBJ_FLAG_HIDDEN)) return;

    const int8_t step = 5;
    char buf[16];

    if (direction > 0) {
        if (settings_focus == 0 && vol_slider) {
            int32_t val = lv_slider_get_value(vol_slider) + step;
            if (val > 100) val = 100;
            lv_slider_set_value(vol_slider, val, LV_ANIM_ON);
            snprintf(buf, sizeof(buf), "%d%%", (int)val);
            lv_label_set_text(vol_label, buf);
            if (on_vol_change_cb) on_vol_change_cb((uint8_t)val);
        } else if (settings_focus == 1 && bright_slider) {
            int32_t val = lv_slider_get_value(bright_slider) + step;
            if (val > 100) val = 100;
            lv_slider_set_value(bright_slider, val, LV_ANIM_ON);
            snprintf(buf, sizeof(buf), "%d%%", (int)val);
            lv_label_set_text(bright_label, buf);
            if (on_bright_change_cb) on_bright_change_cb((uint8_t)val);
        }
    } else {
        if (settings_focus == 0 && vol_slider) {
            int32_t val = lv_slider_get_value(vol_slider) - step;
            if (val < 0) {
                settings_focus = 1;
                lv_obj_set_style_outline_width(bright_slider, 2, 0);
                lv_obj_set_style_outline_color(bright_slider, COLOR_ACCENT, 0);
                lv_obj_set_style_outline_width(vol_slider, 0, 0);
            } else {
                lv_slider_set_value(vol_slider, val, LV_ANIM_ON);
                snprintf(buf, sizeof(buf), "%d%%", (int)val);
                lv_label_set_text(vol_label, buf);
                if (on_vol_change_cb) on_vol_change_cb((uint8_t)val);
            }
        } else if (settings_focus == 1 && bright_slider) {
            int32_t val = lv_slider_get_value(bright_slider) - step;
            if (val < 0) {
                settings_focus = 0;
                lv_obj_set_style_outline_width(vol_slider, 2, 0);
                lv_obj_set_style_outline_color(vol_slider, COLOR_ACCENT, 0);
                lv_obj_set_style_outline_width(bright_slider, 0, 0);
            } else {
                lv_slider_set_value(bright_slider, val, LV_ANIM_ON);
                snprintf(buf, sizeof(buf), "%d%%", (int)val);
                lv_label_set_text(bright_label, buf);
                if (on_bright_change_cb) on_bright_change_cb((uint8_t)val);
            }
        }
    }
}

void ui_set_volume(uint8_t volume)
{
    if (vol_slider) {
        lv_slider_set_value(vol_slider, volume, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", volume);
        lv_label_set_text(vol_label, buf);
    }
}

void ui_set_brightness(uint8_t brightness)
{
    if (bright_slider) {
        lv_slider_set_value(bright_slider, brightness, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", brightness);
        lv_label_set_text(bright_label, buf);
    }
}

void ui_set_task_image(const uint16_t *pixels, uint16_t w, uint16_t h)
{
    if (!task_img_obj || !pixels) return;

    size_t buf_size = (size_t)w * h * sizeof(lv_color_t);

    if (task_img_buf) {
        size_t current_size = (size_t)task_img_dsc.header.w * task_img_dsc.header.h * sizeof(lv_color_t);
        if (current_size < buf_size) {
            heap_caps_free(task_img_buf);
            task_img_buf = NULL;
        }
    }

    if (!task_img_buf) {
        task_img_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!task_img_buf) {
            ESP_LOGE(TAG, "Failed to allocate task image buffer (%zu bytes)", buf_size);
            return;
        }
    }

    memcpy(task_img_buf, pixels, buf_size);

    task_img_dsc.header.always_zero = 0;
    task_img_dsc.header.w = w;
    task_img_dsc.header.h = h;
    task_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    task_img_dsc.data_size = buf_size;
    task_img_dsc.data = (const uint8_t *)task_img_buf;

    lv_img_set_src(task_img_obj, &task_img_dsc);
}

void ui_set_task_status(const char *text)
{
    if (task_status_label) {
        lv_label_set_text(task_status_label, text);
    }
}

/* ========== Playing Screen API ========== */

void ui_show_playing(uint8_t track)
{
    if (scr_playing) {
        lv_scr_load(scr_playing);
        current_state = UI_STATE_PLAYING;
        
        char buf[32];
        snprintf(buf, sizeof(buf), "Track: %d", track);
        if (track_label) {
            lv_label_set_text(track_label, buf);
        }
        if (play_status_label) {
            lv_label_set_text(play_status_label, LV_SYMBOL_PLAY " Playing");
        }
    }
}

void ui_show_stopped(void)
{
    if (scr_stopped) {
        lv_scr_load(scr_stopped);
        current_state = UI_STATE_STOPPED;
    }
}

void ui_set_image(const uint16_t *pixels, uint16_t w, uint16_t h)
{
    if (!play_img_obj || !pixels) return;

    size_t buf_size = (size_t)w * h * sizeof(lv_color_t);

    if (play_img_buf) {
        size_t current_size = (size_t)play_img_dsc.header.w * play_img_dsc.header.h * sizeof(lv_color_t);
        if (current_size < buf_size) {
            heap_caps_free(play_img_buf);
            play_img_buf = NULL;
        }
    }

    if (!play_img_buf) {
        play_img_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!play_img_buf) {
            ESP_LOGE(TAG, "Failed to allocate play image buffer (%zu bytes)", buf_size);
            return;
        }
    }

    memcpy(play_img_buf, pixels, buf_size);

    play_img_dsc.header.always_zero = 0;
    play_img_dsc.header.w = w;
    play_img_dsc.header.h = h;
    play_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    play_img_dsc.data_size = buf_size;
    play_img_dsc.data = (const uint8_t *)play_img_buf;

    lv_img_set_src(play_img_obj, &play_img_dsc);
}

void ui_set_track(uint8_t track)
{
    if (track_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Track: %d", track);
        lv_label_set_text(track_label, buf);
    }
}

ui_state_t ui_get_state(void)
{
    return current_state;
}
