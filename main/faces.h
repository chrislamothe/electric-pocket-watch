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
    lv_obj_t *(*create)(lv_obj_t *parent, int unused);  /* returns the face's root container */
    void (*update)(const struct tm *now);               /* called once per second */
} watch_face_t;


void faces_init(void);

/* Every dial is drawn in a square of side *size (the shorter display side) whose top-left corner
 * is at (*x, *y): the whole round 466 px panel on the 1.75 boards, centred vertically on the
 * 410x502 panel of the 2.06. Faces create their root object there and size everything from
 * *size. faces_place_root() applies it to a face root (position, size, and a click area that
 * reaches the rest of the screen so swipes work anywhere). */
void faces_dial_geometry(int *size, int *x, int *y);
void faces_place_root(lv_obj_t *root);
void faces_next(void);
void faces_prev(void);

/* Pause/resume redraws while the screen is off. Resume draws the current time immediately. */
void faces_set_active(bool active);

/* Re-read the battery-ring setting and level (caller holds the LVGL lock). */
void faces_battery_ring_changed_nolock(void);

/* Re-read the battery level now (USB plugged / unplugged). Takes the LVGL lock. */
void faces_battery_refresh(void);

/* Message box over the face; hide_after_ms <= 0 keeps it until the next call. */
void faces_show_status(const char *text, int hide_after_ms);

extern const watch_face_t face_roman;
extern const watch_face_t face_arabic;
extern const watch_face_t face_black;
extern const watch_face_t face_calendar;
extern const watch_face_t face_railroad;
extern const watch_face_t face_railroad_night;
