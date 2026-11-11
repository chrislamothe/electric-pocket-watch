#include "faces.h"

#include <math.h>
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
static lv_obj_t *s_root = NULL;
static lv_timer_t *s_timer = NULL;
static int s_last_sec = -1;

/* ---------------------------------------------------------------- battery indicator
 * Twelve segments at the edge of the glass, one per hour: 12-1 is the first, 11-12 the last, so a
 * full battery lights all twelve. Each segment is 1/12 of the charge (rounded up). When only the
 * 12-1 and 1-2 segments are left (<= 2/12, about 17 %), the lit segments turn red.
 *
 * One small object per segment, sized to that segment's bounding box, so a change redraws only
 * the edge and not the whole screen. Every refresh (30 s, and on every wake) re-invalidates all
 * twelve whether or not the level changed: if a display transfer was ever lost, the ring repairs
 * itself instead of keeping stale segments. */
#define BATT_SEGMENTS    12
#define BATT_RED_AT      2
#define BATT_W           6
#define BATT_GAP_DEG     2
#define COL_BATT_OK      0x6FB35A
#define COL_BATT_LOW     0xD0342C
#define COL_BATT_CHG     0x4FA3E0     /* charging */
#define BATT_PERIOD_US   (30LL * 1000 * 1000)
#define BATT_HYST_PCT    2

static lv_obj_t *s_seg[BATT_SEGMENTS];
static int s_batt_lit = -1;
static bool s_batt_chg;              /* on USB and still charging */
static int64_t s_batt_next_us;
static int s_batt_cx, s_batt_cy, s_batt_r;

/* LVGL angles: 0 = 3 o'clock, clockwise; 12 o'clock = 270 */
static void seg_angles(int i, int *start, int *end)
{
    *start = (270 + i * 30 + BATT_GAP_DEG) % 360;
    *end = (270 + (i + 1) * 30 - BATT_GAP_DEG) % 360;
}

static void seg_draw_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_batt_lit) {
        return;                                         /* unlit: the face shows through */
    }
    int start, end;
    seg_angles(i, &start, &end);
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_color_hex(s_batt_chg ? COL_BATT_CHG : s_batt_lit <= BATT_RED_AT ? COL_BATT_LOW : COL_BATT_OK);
    d.width = BATT_W;
    d.center.x = s_batt_cx;                             /* absolute: layers use screen coordinates */
    d.center.y = s_batt_cy;
    d.radius = (uint16_t)s_batt_r;
    d.rounded = 0;
    d.start_angle = (lv_value_precise_t)start;
    d.end_angle = (lv_value_precise_t)end;
    lv_draw_arc(lv_event_get_layer(e), &d);
}

static void batt_refresh(bool force)
{
    if (!s_seg[0]) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!force && now < s_batt_next_us) {
        return;
    }
    s_batt_next_us = now + BATT_PERIOD_US;
    /* On USB the board runs from VBUS, so a live reading is fine (it reads high while charging:
     * the charger raises the cell voltage). "Charge complete" from the PMIC shows a full ring. */
    bool usb = axp_vbus_good();
    axp_charge_t cs = usb ? axp_charge_state() : AXP_CHG_DISCHARGING;
    if (usb) {
        axp_rest_update();
    }
    s_batt_chg = usb && cs == AXP_CHG_CHARGING;
    int pct = axp_rest_percent();
    if (usb && cs == AXP_CHG_STANDBY && pct >= 0) {
        pct = 100;
    }
    int lit = 0;
    if (settings_get_int("batt_ring", 1) && pct >= 0) {
        lit = (pct * BATT_SEGMENTS + 99) / 100;         /* any charge in a segment lights it */
        if (lit > BATT_SEGMENTS) {
            lit = BATT_SEGMENTS;
        }
        /* Hysteresis: drop a segment only once the charge is BATT_HYST_PCT below that segment's
         * threshold, so a reading that wobbles across it (load recovery, ADC noise) cannot make
         * the count differ from one wake or face to the next. Rising (charging) is immediate. */
        if (s_batt_lit > 0 && lit == s_batt_lit - 1 &&
            pct * BATT_SEGMENTS + BATT_HYST_PCT * BATT_SEGMENTS > (s_batt_lit - 1) * 100) {
            lit = s_batt_lit;
        }
    }
    if (lit != s_batt_lit) {
        ESP_LOGI(TAG, "battery indicator %d/12 (%d%%, %d mV at rest%s)", lit, pct, axp_rest_mv(),
                 s_batt_chg ? ", charging" : "");
    }
    s_batt_lit = lit;
    for (int i = 0; i < BATT_SEGMENTS; i++) {
        lv_obj_invalidate(s_seg[i]);
    }
}

static void batt_raise(void)
{
    for (int i = 0; i < BATT_SEGMENTS; i++) {
        lv_obj_move_foreground(s_seg[i]);
    }
}

static void batt_create(void)
{
    int w = lv_display_get_horizontal_resolution(NULL);
    s_batt_cx = w / 2;
    s_batt_cy = lv_display_get_vertical_resolution(NULL) / 2;
    s_batt_r = w / 2 - 2;
    for (int i = 0; i < BATT_SEGMENTS; i++) {
        /* bounding box of the segment: sample both edges along the arc */
        int start, end;
        seg_angles(i, &start, &end);
        if (end < start) {
            end += 360;
        }
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (int a = start; a <= end; a++) {
            float rad = a * (float)M_PI / 180.0f;
            for (int k = 0; k < 2; k++) {
                float r = k ? s_batt_r : s_batt_r - BATT_W;
                float x = s_batt_cx + r * cosf(rad), y = s_batt_cy + r * sinf(rad);
                minx = fminf(minx, x);
                maxx = fmaxf(maxx, x);
                miny = fminf(miny, y);
                maxy = fmaxf(maxy, y);
            }
        }
        lv_obj_t *o = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(o);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);   /* touches go to the face */
        lv_obj_set_pos(o, (int)minx - 2, (int)miny - 2);
        lv_obj_set_size(o, (int)(maxx - minx) + 5, (int)(maxy - miny) + 5);
        lv_obj_add_event_cb(o, seg_draw_cb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)i);
        s_seg[i] = o;
    }
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
    s_root = s_faces[s_index]->create(lv_screen_active(), 0);
    lv_obj_add_event_cb(s_root, gesture_cb, LV_EVENT_GESTURE, NULL);
    ESP_LOGI(TAG, "face '%s'", s_faces[s_index]->name);
    tick(true);
    if (s_seg[0]) {
        batt_raise();
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

void faces_battery_refresh(void)
{
    if (bsp_display_lock(1000) != ESP_OK) {
        return;
    }
    batt_refresh(true);
    bsp_display_unlock();
}
