#include "faces.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "axp.h"
#include "bsp/esp-bsp.h"
#include "settings.h"
#include "settings_ui.h"
#include "watch_time.h"

static const char *TAG = "faces";

static const watch_face_t *const s_faces[] = {&face_roman, &face_arabic, &face_black, &face_calendar, &face_railroad, &face_railroad_night};
#define FACE_COUNT ((int)(sizeof(s_faces) / sizeof(s_faces[0])))

static int s_index = 0;
static int s_theme = 0;
static lv_obj_t *s_root = NULL;
static lv_timer_t *s_timer = NULL;
static int s_last_sec = -1;

/* ---------------------------------------------------------------- battery ring
 * Thin ring at the very edge of the glass, above every face: sweep = battery %, clockwise from
 * 12 (100 % = full circle). At 10 % and below it turns red. Read every 30 s and on every wake. */
#define BATT_RING_W      5
#define COL_BATT_OK      0x6FB35A
#define COL_BATT_LOW     0xD0342C
#define COL_BATT_TRACK   0x202020
#define BATT_PERIOD_US   (30LL * 1000 * 1000)

static lv_obj_t *s_batt;
static int64_t s_batt_next_us;

static void batt_refresh(bool force)
{
    if (!s_batt) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!force && now < s_batt_next_us) {
        return;
    }
    s_batt_next_us = now + BATT_PERIOD_US;
    bool enabled = settings_get_int("batt_ring", 1) != 0;
    int pct = axp_battery_percent();
    if (!enabled || pct < 0) {
        lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(s_batt, pct);
    lv_obj_set_style_arc_color(s_batt, lv_color_hex(pct <= 10 ? COL_BATT_LOW : COL_BATT_OK), LV_PART_INDICATOR);
}

static void batt_create(void)
{
    s_batt = lv_arc_create(lv_screen_active());
    lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_CLICKABLE);      /* touches go to the face below */
    lv_obj_remove_style(s_batt, NULL, LV_PART_KNOB);
    lv_obj_set_size(s_batt, lv_display_get_horizontal_resolution(NULL), lv_display_get_vertical_resolution(NULL));
    lv_obj_center(s_batt);
    lv_obj_set_style_pad_all(s_batt, 0, LV_PART_MAIN);
    lv_arc_set_rotation(s_batt, 270);                        /* start at 12 o'clock */
    lv_arc_set_bg_angles(s_batt, 0, 360);
    lv_arc_set_range(s_batt, 0, 100);
    lv_obj_set_style_arc_width(s_batt, BATT_RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_batt, BATT_RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_batt, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_batt, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_batt, lv_color_hex(COL_BATT_TRACK), LV_PART_MAIN);
    batt_refresh(true);
}

static void tick(bool force)
{
    struct tm now;
    watch_time_now(&now, NULL);
    if (!force && now.tm_sec == s_last_sec) {
        return;
    }
    s_last_sec = now.tm_sec;
    if (s_root) {
        s_faces[s_index]->update(&now);
    }
    batt_refresh(force);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    tick(false);
}

static void gesture_cb(lv_event_t *e);
static lv_obj_t *s_status = NULL;
static lv_timer_t *s_status_timer = NULL;

static void build_current(void)
{
    if (s_root) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_root = s_faces[s_index]->create(lv_screen_active(), s_theme);
    lv_obj_add_event_cb(s_root, gesture_cb, LV_EVENT_GESTURE, NULL);
    ESP_LOGI(TAG, "face '%s', theme %d", s_faces[s_index]->name, s_theme);
    tick(true);
    if (s_batt) {
        lv_obj_move_foreground(s_batt);
    }
    if (s_status) {
        lv_obj_move_foreground(s_status);
    }
}

static void select_face(int step)
{
    s_index = (s_index + step + FACE_COUNT) % FACE_COUNT;
    build_current();
    settings_set_int("face", s_index);
}

static void async_step(void *arg)
{
    select_face((int)(intptr_t)arg);
}

static void open_settings_cb(void *arg)
{
    (void)arg;
    settings_ui_open_nolock();
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP) {
        lv_async_call(open_settings_cb, NULL);
        return;
    }
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        /* Deleting the face from inside its own event callback is unsafe: defer. */
        lv_async_call(async_step, (void *)(intptr_t)(dir == LV_DIR_LEFT ? 1 : -1));
    }
}

void faces_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_theme = settings_get_int("theme", 0);
    if (s_theme < 0 || s_theme >= WATCH_THEME_COUNT) {
        s_theme = 0;
    }
    s_index = settings_get_int("face", 0);
    if (s_index < 0 || s_index >= FACE_COUNT) {
        s_index = 0;
    }
    batt_create();
    build_current();
    s_timer = lv_timer_create(timer_cb, 250, NULL);
}

static void step_locked(int step)
{
    if (bsp_display_lock(500) != ESP_OK) {
        return;
    }
    select_face(step);
    bsp_display_unlock();
}

void faces_next(void) { step_locked(1); }
void faces_prev(void) { step_locked(-1); }

void faces_set_theme_nolock(int theme)
{
    s_theme = ((theme % WATCH_THEME_COUNT) + WATCH_THEME_COUNT) % WATCH_THEME_COUNT;
    build_current();
}

void faces_set_theme(int theme)
{
    if (bsp_display_lock(500) != ESP_OK) {
        return;
    }
    faces_set_theme_nolock(theme);
    bsp_display_unlock();
}

int faces_theme(void) { return s_theme; }

void faces_set_active(bool active)
{
    if (bsp_display_lock(500) != ESP_OK) {
        return;
    }
    if (active) {
        tick(true);
        lv_obj_invalidate(lv_screen_active());  /* full repaint on wake: no stale panel content */
        lv_timer_resume(s_timer);
    } else {
        lv_timer_pause(s_timer);
    }
    bsp_display_unlock();
}

/* ---------------------------------------------------------------- status message */

static void status_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_status) {
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    }
    s_status_timer = NULL;                      /* one-shot timer deletes itself */
}

void faces_show_status(const char *text, int hide_after_ms)
{
    if (!s_timer || bsp_display_lock(1000) != ESP_OK) {
        return;                                 /* not initialised yet, or LVGL busy */
    }
    if (!s_status) {
        s_status = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s_status, lv_color_white(), 0);
        lv_obj_set_style_bg_color(s_status, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_status, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_status, 14, 0);
        lv_obj_set_style_pad_hor(s_status, 18, 0);
        lv_obj_set_style_pad_ver(s_status, 10, 0);
    }
    lv_label_set_text(s_status, text);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 120);
    lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_status);
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = NULL;
    }
    if (hide_after_ms > 0) {
        s_status_timer = lv_timer_create(status_hide_cb, hide_after_ms, NULL);
        lv_timer_set_repeat_count(s_status_timer, 1);
    }
    bsp_display_unlock();
}

void faces_battery_ring_changed_nolock(void)
{
    batt_refresh(true);
}
