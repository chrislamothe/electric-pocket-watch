#include "faces.h"

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "watch_time.h"

static const char *TAG = "faces";

static const watch_face_t *const s_faces[] = {&face_analog, &face_digital};
#define FACE_COUNT ((int)(sizeof(s_faces) / sizeof(s_faces[0])))

static int s_index = 0;
static int s_theme = 0;
static lv_obj_t *s_root = NULL;
static lv_timer_t *s_timer = NULL;
static int s_last_sec = -1;

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
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    tick(false);
}

static void gesture_cb(lv_event_t *e);

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
}

static void async_step(void *arg)
{
    int step = (int)(intptr_t)arg;
    s_index = (s_index + step + FACE_COUNT) % FACE_COUNT;
    build_current();
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
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
    build_current();
    s_timer = lv_timer_create(timer_cb, 250, NULL);
}

static void step_locked(int step)
{
    if (bsp_display_lock(500) != ESP_OK) {
        return;
    }
    s_index = (s_index + step + FACE_COUNT) % FACE_COUNT;
    build_current();
    bsp_display_unlock();
}

void faces_next(void) { step_locked(1); }
void faces_prev(void) { step_locked(-1); }

void faces_set_theme(int theme)
{
    if (bsp_display_lock(500) != ESP_OK) {
        return;
    }
    s_theme = ((theme % WATCH_THEME_COUNT) + WATCH_THEME_COUNT) % WATCH_THEME_COUNT;
    build_current();
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
        lv_timer_resume(s_timer);
    } else {
        lv_timer_pause(s_timer);
    }
    bsp_display_unlock();
}
