/*
 * ui_main.c - LVGL UI implementation (v8.3)
 * Landscape 320x240, main menu with Play/USB/Settings buttons
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
static lv_obj_t *scr_playing = NULL;
static lv_obj_t *scr_stopped = NULL;
static lv_obj_t *settings_panel = NULL;

/* Welcome screen objects */
static lv_obj_t *welcome_bar = NULL;

/* Menu objects */
static lv_obj_t *menu_btns[MENU_ITEM_COUNT];
static lv_obj_t *menu_labels[MENU_ITEM_COUNT];
static menu_item_t menu_selection = MENU_ITEM_PLAY;

/* Playing screen objects */
static lv_obj_t *img_obj = NULL;
static lv_obj_t *track_label = NULL;
static lv_obj_t *btn_play_ctrl = NULL;
static lv_obj_t *btn_prev_ctrl = NULL;
static lv_obj_t *btn_next_ctrl = NULL;
static lv_obj_t *ctrl_bar = NULL;

/* Settings objects */
static lv_obj_t *vol_slider = NULL;
static lv_obj_t *bright_slider = NULL;
static lv_obj_t *vol_label = NULL;
static lv_obj_t *bright_label = NULL;

/* Image buffer */
static lv_color_t *img_buf = NULL;
static lv_img_dsc_t img_dsc = {0};

/* Callbacks */
static void (*on_prev_cb)(void) = NULL;
static void (*on_play_cb)(void) = NULL;
static void (*on_next_cb)(void) = NULL;
static void (*on_vol_change_cb)(uint8_t) = NULL;
static void (*on_bright_change_cb)(uint8_t) = NULL;
static void (*on_usb_cb)(void) = NULL;

/* Control bar auto-hide timer */
static lv_timer_t *ctrl_hide_timer = NULL;

/* Previous state for settings return */
static ui_state_t prev_state = UI_STATE_MENU;

/* Settings focus: 0=volume, 1=brightness */
static uint8_t settings_focus = 0;

/* Task buttons */
static lv_obj_t *btn_task1 = NULL;
static lv_obj_t *btn_task2 = NULL;
static lv_obj_t *status_label = NULL;
static ui_task_btn_cb_t on_task1_cb = NULL;
static ui_task_btn_cb_t on_task2_cb = NULL;

/* Forward declarations */
static void update_menu_focus(void);
static void btn_task1_handler(lv_event_t *e);
static void btn_task2_handler(lv_event_t *e);

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

/* ========== Menu Button Handlers ========== */
/* Callback for menu item execution (set by main.c) */
static void (*on_menu_play_cb)(void) = NULL;
static void (*on_menu_usb_cb)(void) = NULL;
static void (*on_menu_settings_cb)(void) = NULL;

void ui_set_menu_callbacks(void (*play)(void), void (*usb)(void), void (*settings)(void))
{
    on_menu_play_cb = play;
    on_menu_usb_cb = usb;
    on_menu_settings_cb = settings;
}

static void menu_btn_handler(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        if (btn == menu_btns[i]) {
            menu_selection = (menu_item_t)i;
            update_menu_focus(); /* Update visual immediately */
            
            /* Execute the corresponding action */
            switch (menu_selection) {
                case MENU_ITEM_PLAY:
                    if (on_menu_play_cb) on_menu_play_cb();
                    break;
                case MENU_ITEM_USB:
                    if (on_menu_usb_cb) on_menu_usb_cb();
                    break;
                case MENU_ITEM_SETTINGS:
                    if (on_menu_settings_cb) on_menu_settings_cb();
                    break;
                default:
                    break;
            }
            break;
        }
    }
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
        if (ctrl_hide_timer) {
            lv_timer_reset(ctrl_hide_timer);
        } else {
            ctrl_hide_timer = lv_timer_create(ctrl_hide_timer_cb, 3000, NULL);
            lv_timer_set_repeat_count(ctrl_hide_timer, 1);
        }
    }
}

/* ========== Control Button Handlers ========== */
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

static void btn_task1_handler(lv_event_t *e)
{
    (void)e;
    if (on_task1_cb) {
        on_task1_cb();
        if (status_label) {
            lv_label_set_text(status_label, "Task 1 Sent");
        }
    }
}

static void btn_task2_handler(lv_event_t *e)
{
    (void)e;
    if (on_task2_cb) {
        on_task2_cb();
        if (status_label) {
            lv_label_set_text(status_label, "Task 2 Sent");
        }
    }
}

static void close_settings_handler(lv_event_t *e)
{
    ui_hide_settings();
}

/* ========== Create Welcome Screen ========== */
static void create_welcome_screen(void)
{
    scr_welcome = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_welcome, COLOR_BG, 0);

    lv_obj_t *welcome_label = lv_label_create(scr_welcome);
    lv_label_set_text(welcome_label, "ESP32-S3\nMP3 Player");
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

    /* Title - Top area */
    lv_obj_t *title = lv_label_create(scr_menu);
    lv_label_set_text(title, "MP3 Player");
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Menu buttons - Center area (vertical layout) */
    static const char *menu_texts[] = { LV_SYMBOL_PLAY " Play", LV_SYMBOL_USB " USB", LV_SYMBOL_SETTINGS " Settings" };
    int btn_y_start = 50;
    int btn_h = 40;
    int btn_gap = 8;

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        menu_btns[i] = lv_btn_create(scr_menu);
        lv_obj_set_size(menu_btns[i], 240, btn_h);
        lv_obj_align(menu_btns[i], LV_ALIGN_TOP_MID, 0, btn_y_start + i * (btn_h + btn_gap));
        lv_obj_set_style_bg_color(menu_btns[i], COLOR_SURFACE, 0);
        lv_obj_set_style_bg_color(menu_btns[i], COLOR_SELECTED, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(menu_btns[i], 2, 0);
        lv_obj_set_style_border_color(menu_btns[i], COLOR_PRIMARY, 0);
        lv_obj_set_style_border_color(menu_btns[i], COLOR_ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(menu_btns[i], 6, 0);
        lv_obj_add_event_cb(menu_btns[i], menu_btn_handler, LV_EVENT_CLICKED, NULL);

        menu_labels[i] = lv_label_create(menu_btns[i]);
        lv_label_set_text(menu_labels[i], menu_texts[i]);
        lv_obj_set_style_text_color(menu_labels[i], COLOR_TEXT, 0);
        lv_obj_center(menu_labels[i]);
    }

    /* Bottom area - Task buttons and status */
    /* Task button 1 */
    btn_task1 = lv_btn_create(scr_menu);
    lv_obj_set_size(btn_task1, 80, 28);
    lv_obj_align(btn_task1, LV_ALIGN_BOTTOM_LEFT, 10, -45);
    lv_obj_set_style_bg_color(btn_task1, lv_color_hex(0x2D5F8A), 0);
    lv_obj_set_style_bg_color(btn_task1, COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn_task1, 4, 0);
    lv_obj_add_event_cb(btn_task1, btn_task1_handler, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *lbl_task1 = lv_label_create(btn_task1);
    lv_label_set_text(lbl_task1, "Task1");
    lv_obj_set_style_text_font(lbl_task1, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl_task1);
    
    /* Task button 2 */
    btn_task2 = lv_btn_create(scr_menu);
    lv_obj_set_size(btn_task2, 80, 28);
    lv_obj_align(btn_task2, LV_ALIGN_BOTTOM_RIGHT, -10, -45);
    lv_obj_set_style_bg_color(btn_task2, lv_color_hex(0x2D5F8A), 0);
    lv_obj_set_style_bg_color(btn_task2, COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn_task2, 4, 0);
    lv_obj_add_event_cb(btn_task2, btn_task2_handler, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *lbl_task2 = lv_label_create(btn_task2);
    lv_label_set_text(lbl_task2, "Task2");
    lv_obj_set_style_text_font(lbl_task2, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl_task2);
    
    /* Status label - centered between task buttons */
    status_label = lv_label_create(scr_menu);
    lv_label_set_text(status_label, "Ready");
    lv_obj_set_style_text_color(status_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_10, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -15);

    /* Hint text - very bottom */
    lv_obj_t *hint = lv_label_create(scr_menu);
    lv_label_set_text(hint, "GPIO: Up/Down");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_8, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    /* Focus first item */
    lv_group_focus_obj(menu_btns[0]);
}

/* ========== Update Menu Selection Visual ========== */
static void update_menu_focus(void)
{
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        if (i == menu_selection) {
            lv_obj_add_state(menu_btns[i], LV_STATE_FOCUSED);
            lv_obj_set_style_bg_color(menu_btns[i], COLOR_SELECTED, 0);
            lv_obj_set_style_border_color(menu_btns[i], COLOR_ACCENT, 0);
        } else {
            lv_obj_clear_state(menu_btns[i], LV_STATE_FOCUSED);
            lv_obj_set_style_bg_color(menu_btns[i], COLOR_SURFACE, 0);
            lv_obj_set_style_border_color(menu_btns[i], COLOR_PRIMARY, 0);
        }
    }
}

/* ========== Create Playing Screen ========== */
static void create_playing_screen(void)
{
    scr_playing = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_playing, COLOR_BG, 0);

    /* Full-screen image */
    img_obj = lv_img_create(scr_playing);
    lv_obj_set_size(img_obj, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_align(img_obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(img_obj, lv_color_hex(0x000000), 0);

    /* Track label overlay */
    track_label = lv_label_create(scr_playing);
    lv_label_set_text(track_label, "Track 1");
    lv_obj_set_style_text_color(track_label, COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(track_label, lv_color_hex(0x80000000), 0);
    lv_obj_set_style_pad_all(track_label, 4, 0);
    lv_obj_align(track_label, LV_ALIGN_TOP_LEFT, 8, 8);

    /* Bottom control bar (hidden by default) */
    ctrl_bar = lv_obj_create(scr_playing);
    lv_obj_set_size(ctrl_bar, DISPLAY_WIDTH, 50);
    lv_obj_align(ctrl_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ctrl_bar, lv_color_hex(0xAA16213E), 0);
    lv_obj_set_style_border_width(ctrl_bar, 0, 0);
    lv_obj_set_style_radius(ctrl_bar, 0, 0);
    lv_obj_add_flag(ctrl_bar, LV_OBJ_FLAG_HIDDEN);

    btn_prev_ctrl = lv_btn_create(ctrl_bar);
    lv_obj_set_size(btn_prev_ctrl, 50, 36);
    lv_obj_align(btn_prev_ctrl, LV_ALIGN_LEFT_MID, 15, 0);
    lv_obj_set_style_bg_color(btn_prev_ctrl, COLOR_PRIMARY, 0);
    lv_obj_t *prev_lbl = lv_label_create(btn_prev_ctrl);
    lv_label_set_text(prev_lbl, LV_SYMBOL_PREV);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(btn_prev_ctrl, btn_prev_handler, LV_EVENT_CLICKED, NULL);

    btn_play_ctrl = lv_btn_create(ctrl_bar);
    lv_obj_set_size(btn_play_ctrl, 50, 36);
    lv_obj_align(btn_play_ctrl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(btn_play_ctrl, COLOR_ACCENT, 0);
    lv_obj_t *play_lbl = lv_label_create(btn_play_ctrl);
    lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
    lv_obj_center(play_lbl);
    lv_obj_add_event_cb(btn_play_ctrl, btn_play_handler, LV_EVENT_CLICKED, NULL);

    btn_next_ctrl = lv_btn_create(ctrl_bar);
    lv_obj_set_size(btn_next_ctrl, 50, 36);
    lv_obj_align(btn_next_ctrl, LV_ALIGN_RIGHT_MID, -15, 0);
    lv_obj_set_style_bg_color(btn_next_ctrl, COLOR_PRIMARY, 0);
    lv_obj_t *next_lbl = lv_label_create(btn_next_ctrl);
    lv_label_set_text(next_lbl, LV_SYMBOL_NEXT);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(btn_next_ctrl, btn_next_handler, LV_EVENT_CLICKED, NULL);
}

/* ========== Create Stopped Screen ========== */
static void create_stopped_screen(void)
{
    scr_stopped = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_stopped, COLOR_BG, 0);

    lv_obj_t *stop_label = lv_label_create(scr_stopped);
    lv_label_set_text(stop_label, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(stop_label, COLOR_ACCENT, 0);
    lv_obj_align(stop_label, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *stop_hint = lv_label_create(scr_stopped);
    lv_label_set_text(stop_hint, "Press Play to start");
    lv_obj_set_style_text_color(stop_hint, COLOR_MUTED, 0);
    lv_obj_align(stop_hint, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *track_info = lv_label_create(scr_stopped);
    lv_label_set_text(track_info, "Track 1");
    lv_obj_set_style_text_color(track_info, COLOR_TEXT, 0);
    lv_obj_align(track_info, LV_ALIGN_CENTER, 0, 50);
}

/* ========== Create Settings Panel ========== */
static void create_settings_panel(void)
{
    /* Use lv_layer_top() so settings panel is always visible above any screen */
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
    ESP_LOGI(TAG, "Creating UI screens (landscape 320x240)...");

    create_welcome_screen();
    create_menu_screen();
    create_playing_screen();
    create_stopped_screen();
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
        menu_selection = MENU_ITEM_PLAY;
        update_menu_focus();
    }
}

void ui_menu_navigate(int8_t direction)
{
    if (current_state != UI_STATE_MENU) return;
    int new_sel = menu_selection + direction;
    if (new_sel < 0) new_sel = MENU_ITEM_COUNT - 1;
    if (new_sel >= MENU_ITEM_COUNT) new_sel = 0;
    menu_selection = (menu_item_t)new_sel;
    update_menu_focus();
}

menu_item_t ui_menu_confirm(void)
{
    return menu_selection;
}

menu_item_t ui_menu_get_selection(void)
{
    return menu_selection;
}

void ui_show_playing(uint8_t track)
{
    if (scr_playing) {
        lv_scr_load(scr_playing);
        current_state = UI_STATE_PLAYING;
        char buf[16];
        snprintf(buf, sizeof(buf), "Track %d", track);
        lv_label_set_text(track_label, buf);
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
        prev_state = current_state;
        lv_obj_clear_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(settings_panel);
        current_state = UI_STATE_SETTINGS;
        /* Reset focus to volume and highlight it */
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
        settings_focus = 0; /* Reset focus to volume */
    }
}

void ui_settings_adjust(int8_t direction)
{
    if (!settings_panel || lv_obj_has_flag(settings_panel, LV_OBJ_FLAG_HIDDEN)) return;

    const int8_t step = 5; /* Adjust by 5% per press */
    char buf[16]; /* Large enough for any int value */

    if (direction > 0) {
        /* GPIO43 short press: increase focused slider */
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
        /* GPIO0 short press: decrease focused slider, or switch focus */
        if (settings_focus == 0 && vol_slider) {
            int32_t val = lv_slider_get_value(vol_slider) - step;
            if (val < 0) {
                /* Already at min, switch focus to brightness */
                settings_focus = 1;
                /* Highlight brightness slider */
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
                /* Already at min, switch focus to volume */
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
    if (!img_obj || !pixels) return;

    size_t buf_size = (size_t)w * h * sizeof(lv_color_t);

    if (img_buf) {
        size_t current_size = (size_t)img_dsc.header.w * img_dsc.header.h * sizeof(lv_color_t);
        if (current_size < buf_size) {
            heap_caps_free(img_buf);
            img_buf = NULL;
        }
    }

    if (!img_buf) {
        img_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!img_buf) {
            ESP_LOGE(TAG, "Failed to allocate image buffer (%zu bytes)", buf_size);
            return;
        }
    }

    memcpy(img_buf, pixels, buf_size);

    img_dsc.header.always_zero = 0;
    img_dsc.header.w = w;
    img_dsc.header.h = h;
    img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    img_dsc.data_size = buf_size;
    img_dsc.data = (const uint8_t *)img_buf;

    lv_img_set_src(img_obj, &img_dsc);
}

ui_state_t ui_get_state(void)
{
    return current_state;
}

void ui_set_play_button_text(bool is_playing)
{
    if (btn_play_ctrl) {
        lv_obj_t *lbl = lv_obj_get_child(btn_play_ctrl, 0);
        if (lbl) {
            lv_label_set_text(lbl, is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
    }
}

void ui_set_task_status(const char *text)
{
    if (status_label) {
        lv_label_set_text(status_label, text);
    }
}