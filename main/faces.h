#pragma once

#include <stdbool.h>
#include <time.h>
#include "lvgl.h"

/*
 * Face manager. A face is a full-screen LVGL container that the manager creates when the face
 * becomes active and deletes when another face replaces it. The manager owns a 250 ms LVGL
 * timer that calls the active face's update() whenever the wall-clock second changes.
 *
 * Locking: faces_init() is called with the LVGL lock held; every other public function takes
 * the lock itself (BSP lock = esp_lv_adapter lock, not assumed recursive).
 */

typedef struct {
    const char *name;
    lv_obj_t *(*create)(lv_obj_t *parent, int theme);   /* returns the face's root container */
    void (*update)(const struct tm *now);               /* called once per second */
} watch_face_t;

#define WATCH_THEME_COUNT 3     /* 0 brass, 1 silver, 2 dark */

void faces_init(void);
void faces_next(void);
void faces_prev(void);
void faces_set_theme(int theme);
void faces_set_theme_nolock(int theme);   /* caller holds the LVGL lock */
int  faces_theme(void);

/* Pause/resume redraws while the screen is off. Resume draws the current time immediately. */
void faces_set_active(bool active);

/* Re-read the battery-ring setting and level (caller holds the LVGL lock). */
void faces_battery_ring_changed_nolock(void);

/* Message box over the face; hide_after_ms <= 0 keeps it until the next call. */
void faces_show_status(const char *text, int hide_after_ms);

extern const watch_face_t face_roman;
extern const watch_face_t face_arabic;
extern const watch_face_t face_black;
extern const watch_face_t face_calendar;
extern const watch_face_t face_railroad;
extern const watch_face_t face_railroad_night;
