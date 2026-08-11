/*
 * ui_main.c - LVGL UI implementation (v8.3)
 * Landscape 320x240, full-screen image, swipe to open settings
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
static lv_obj_t *scr_playing = NULL;
static lv_obj_t *scr_stopped = NULL;
static lv_obj_t *settings_panel = NULL;

/* Welcome screen objects */
static lv_obj_t *welcome_bar = NULL;

/* Playing screen objects */
static lv_obj_t *img_canvas = NULL;
static lv_obj_t *track_label = NULL;
static lv_obj_t *btn_play = NULL;
static lv_obj_t *btn_prev = NULL;
static lv_obj_t *btn_next = NULL;
static lv_obj_t *ctrl_bar = NULL;

/* Settings objects */
static lv_obj_t *vol_slider = NULL;
static lv_obj_t *bright_slider = NULL;
static lv_obj_t *vol_label = NULL;
static lv_obj_t *bright_label = NULL;

/* Image canvas buffer */
static lv_color_t *img_buf = NULL;
static lv_img_dsc_t img_dsc = {0};

/* Callbacks */
static void (*on_prev_cb)(void) = NULL;
static void (*on_play_cb)(void) = NULL;
static void (*on_next_cb)(void) = NULL;
static void (*on_vol_change_cb)(uint8_t) = NULL;
static void (*on_bright_change_cb)(uint8_t) = NULL;

/* Gesture tracking */
static bool gesture_active = false;
static lv_coord_t gesture_start_x = 0;
static lv_coord_t gesture_start_y = 0;

/* Control bar auto-hide timer */
static lv_timer_t *ctrl_hide_timer = NULL;

/* ========== Color Scheme ========== */
#define COLOR_BG       lv_color_hex(0x1A1A2E)
#define COLOR_SURFACE  lv_color_hex(0x16213E)
#define COLOR_PRIMARY  lv_color_hex(0x0F3460)
#define COLOR_ACCENT   lv_color_hex(0xE94560)
#define COLOR_TEXT     lv_color_hex(0xEAEAEA)
#define COLOR_MUTED    lv_color_hex(0x888888)

/* ========== Callback Setters ========== */
void ui_set_callbacks(void (*prev)(void), void (*play)(void), void (*next)(void),
                      void (*vol)(uint8_t), void (*bright)(uint8_t))
{
    on_prev_cb = prev;
    on_play_cb = play;
    on_next_cb = next;
    on_vol_change_cb = vol;
    on_bright_change_cb = bright;
}

/* ========== Button Event Handlers ========== */
static void btn_prev_handler(lv_event_t *e)
{
    if (on_prev_cb) on_prev_cb();
}

static void btn_play_handler(lv_event_t *e)
{
    if (on_play_cb) on_play_cb();
}

static void btn_next_handler(lv_event_t *e)
{
    if (on_next_cb) on_next_cb();
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

/* ========== Control Bar Auto-Hide ========== */
static void ctrl_hide_timer_cb(lv_timer_t *timer)
{
    if (ctrl_bar) {
        lv_obj_add_flag(ctrl_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_del(ctrl_hide_timer);
    ctrl_hide_timer = NULL;
}

static void show_controls_temporarily(void)
{
    if (ctrl_bar) {
        lv_obj_clear_flag(ctrl_bar, LV_OBJ_FLAG_HIDDEN);
        /* Reset auto-hide timer */
        if (ctrl_hide_timer) {
            lv_timer_reset(ctrl_hide_timer);
        } else {
            ctrl_hide_timer = lv_timer_create(ctrl_hide_timer_cb, 3000, NULL);
            lv_timer_set_repeat_count(ctrl_hide_timer, 1);
        }
    }
}

/* ========== Gesture / Swipe Detection ========== */
static void playing_scr_gesture_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            if (!gesture_active) {
                gesture_start_x = p.x;
                gesture_start_y = p.y;
                gesture_active = true;
            }
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (gesture_active) {
            lv_indev_t *indev = lv_indev_get_act();
            if (indev) {
                lv_point_t p;
                lv_indev_get_point(indev, &p);
                lv_coord_t dx = p.x - gesture_start_x;
                lv_coord_t dy = p.y - gesture_start_y;
                
                /* Swipe right to open settings */
                if (dx > 80 && abs(dy) < 60) {
                    if (on_play_cb) {
                        /* Use play callback as toggle for settings */
                        ui_show_settings();
                    }
                }
                /* Swipe left to toggle controls */
                if (dx < -80 && abs(dy) < 60) {
                    show_controls_temporarily();
                }
            }
            gesture_active = false;
        }
    } else if (code == LV_EVENT_CLICKED) {
        /* Tap on image area to show/hide controls */
        show_controls_temporarily();
    }
}

/* ========== Create Welcome Screen ========== */
static void create_welcome_screen(void)
{
    scr_welcome = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_welcome, COLOR_BG, 0);

    /* Title */
    lv_obj_t *welcome_label = lv_label_create(scr_welcome);
    lv_label_set_text(welcome_label, "ESP32-S3\nMP3 Player");
    lv_obj_set_style_text_color(welcome_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_align(welcome_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(welcome_label, LV_ALIGN_CENTER, 0, -30);

    /* Loading bar */
    welcome_bar = lv_bar_create(scr_welcome);
    lv_obj_set_size(welcome_bar, 200, 6);
    lv_obj_align(welcome_bar, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(welcome_bar, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_color(welcome_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_bar_set_value(welcome_bar, 0, LV_ANIM_OFF);

    /* Loading text */
    lv_obj_t *loading = lv_label_create(scr_welcome);
    lv_label_set_text(loading, "Initializing...");
    lv_obj_set_style_text_color(loading, COLOR_MUTED, 0);
    lv_obj_align(loading, LV_ALIGN_CENTER, 0, 50);
}

/* ========== Create Playing Screen (Full-screen image + swipe) ========== */
static void create_playing_screen(void)
{
    scr_playing = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_playing, COLOR_BG, 0);
    
    /* Full-screen image canvas */
    img_canvas = lv_canvas_create(scr_playing);
    lv_obj_set_size(img_canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_align(img_canvas, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(img_canvas, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(img_canvas, 0, 0);
    lv_obj_set_style_radius(img_canvas, 0, 0);
    
    /* Add gesture handler to the playing screen */
    lv_obj_add_event_cb(scr_playing, playing_scr_gesture_cb, LV_EVENT_ALL, NULL);
    
    /* Track label overlay (top-left corner) */
    track_label = lv_label_create(scr_playing);
    lv_label_set_text(track_label, "Track 1");
    lv_obj_set_style_text_color(track_label, COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(track_label, lv_color_hex(0x80000000), 0);
    lv_obj_set_style_pad_all(track_label, 4, 0);
    lv_obj_align(track_label, LV_ALIGN_TOP_LEFT, 8, 8);
    
    /* Bottom control bar (hidden by default, shown on tap/swipe) */
    ctrl_bar = lv_obj_create(scr_playing);
    lv_obj_set_size(ctrl_bar, DISPLAY_WIDTH, 50);
    lv_obj_align(ctrl_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ctrl_bar, lv_color_hex(0xAA16213E), 0);
    lv_obj_set_style_border_width(ctrl_bar, 0, 0);
    lv_obj_set_style_radius(ctrl_bar, 0, 0);
    lv_obj_add_flag(ctrl_bar, LV_OBJ_FLAG_HIDDEN);

    /* Prev button */
    btn_prev = lv_btn_create(ctrl_bar);
    lv_obj_set_size(btn_prev, 50, 36);
    lv_obj_align(btn_prev, LV_ALIGN_LEFT_MID, 15, 0);
    lv_obj_set_style_bg_color(btn_prev, COLOR_PRIMARY, 0);
    lv_obj_t *prev_lbl = lv_label_create(btn_prev);
    lv_label_set_text(prev_lbl, LV_SYMBOL_PREV);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(btn_prev, btn_prev_handler, LV_EVENT_CLICKED, NULL);

    /* Play/Pause button */
    btn_play = lv_btn_create(ctrl_bar);
    lv_obj_set_size(btn_play, 50, 36);
    lv_obj_align(btn_play, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(btn_play, COLOR_ACCENT, 0);
    lv_obj_t *play_lbl = lv_label_create(btn_play);
    lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
    lv_obj_center(play_lbl);
    lv_obj_add_event_cb(btn_play, btn_play_handler, LV_EVENT_CLICKED, NULL);

    /* Next button */
    btn_next = lv_btn_create(ctrl_bar);
    lv_obj_set_size(btn_next, 50, 36);
    lv_obj_align(btn_next, LV_ALIGN_RIGHT_MID, -15, 0);
    lv_obj_set_style_bg_color(btn_next, COLOR_PRIMARY, 0);
    lv_obj_t *next_lbl = lv_label_create(btn_next);
    lv_label_set_text(next_lbl, LV_SYMBOL_NEXT);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(btn_next, btn_next_handler, LV_EVENT_CLICKED, NULL);
}

/* ========== Create Stopped Screen ========== */
static void create_stopped_screen(void)
{
    scr_stopped = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_stopped, COLOR_BG, 0);

    /* STOP label */
    lv_obj_t *stop_label = lv_label_create(scr_stopped);
    lv_label_set_text(stop_label, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(stop_label, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(stop_label, lv_font_default(), 0);
    lv_obj_align(stop_label, LV_ALIGN_CENTER, 0, -30);

    /* Hint text */
    lv_obj_t *stop_hint = lv_label_create(scr_stopped);
    lv_label_set_text(stop_hint, "Tap Play to start");
    lv_obj_set_style_text_color(stop_hint, COLOR_MUTED, 0);
    lv_obj_align(stop_hint, LV_ALIGN_CENTER, 0, 20);

    /* Track info */
    lv_obj_t *track_info = lv_label_create(scr_stopped);
    lv_label_set_text(track_info, "Track 1");
    lv_obj_set_style_text_color(track_info, COLOR_TEXT, 0);
    lv_obj_align(track_info, LV_ALIGN_CENTER, 0, 50);
}

/* ========== Create Settings Panel ========== */
static void create_settings_panel(void)
{
    settings_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(settings_panel, DISPLAY_WIDTH - 20, 180);
    lv_obj_align(settings_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(settings_panel, COLOR_SURFACE, 0);
    lv_obj_set_style_radius(settings_panel, 10, 0);
    lv_obj_set_style_border_width(settings_panel, 0, 0);
    lv_obj_set_style_shadow_width(settings_panel, 20, 0);
    lv_obj_set_style_shadow_color(settings_panel, lv_color_hex(0x000000), 0);

    /* Title */
    lv_obj_t *title = lv_label_create(settings_panel);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Volume slider */
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

    /* Brightness slider */
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

    /* Close button */
    lv_obj_t *close_btn = lv_btn_create(settings_panel);
    lv_obj_set_size(close_btn, 80, 28);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(close_btn, COLOR_ACCENT, 0);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, btn_play_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ========== Public API ========== */

void ui_init(void)
{
    ESP_LOGI(TAG, "Creating UI screens (landscape 320x240)...");

    create_welcome_screen();
    create_playing_screen();
    create_stopped_screen();
    create_settings_panel();

    /* Show welcome screen by default */
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

void ui_show_playing(uint8_t track)
{
    if (scr_playing) {
        lv_scr_load(scr_playing);
        current_state = UI_STATE_PLAYING;
        char buf[16];
        snprintf(buf, sizeof(buf), "Track %d", track);
        lv_label_set_text(track_label, buf);
        /* Hide settings panel when switching to playing */
        ui_hide_settings();
    }
}

void ui_show_stopped(void)
{
    if (scr_stopped) {
        lv_scr_load(scr_stopped);
        current_state = UI_STATE_STOPPED;
    }
}

void ui_show_settings(void)
{
    if (settings_panel) {
        lv_obj_clear_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(settings_panel);
        current_state = UI_STATE_SETTINGS;
    }
}

void ui_hide_settings(void)
{
    if (settings_panel) {
        lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
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

void ui_set_track(uint8_t track)
{
    if (track_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Track %d", track);
        lv_label_set_text(track_label, buf);
    }
}

void ui_set_image(const uint16_t *pixels, uint16_t w, uint16_t h)
{
    if (!img_canvas || !pixels) return;

    size_t buf_size = (size_t)w * h * sizeof(lv_color_t);
    
    /* Free old buffer if size changed */
    if (img_buf) {
        heap_caps_free(img_buf);
        img_buf = NULL;
    }
    
    img_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img_buf) {
        ESP_LOGE(TAG, "Failed to allocate image buffer (%zu bytes)", buf_size);
        return;
    }

    memcpy(img_buf, pixels, buf_size);

    img_dsc.header.always_zero = 0;
    img_dsc.header.w = w;
    img_dsc.header.h = h;
    img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    img_dsc.data_size = buf_size;
    img_dsc.data = (const uint8_t *)img_buf;

    lv_canvas_set_buffer(img_canvas, img_buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_draw_img(img_canvas, 0, 0, &img_dsc, NULL);
}

ui_state_t ui_get_state(void)
{
    return current_state;
}

void ui_set_play_button_text(bool is_playing)
{
    if (btn_play) {
        lv_obj_t *lbl = lv_obj_get_child(btn_play, 0);
        if (lbl) {
            lv_label_set_text(lbl, is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
    }
}
