#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

/*
 * BOOT = GPIO0, active low (10K pull-up on the board).
 * PWR  = GPIO3 (SYS_OUT, the inverted PWR key), active high. The PMIC powers the board off on a
 * long hold by itself; that is independent of what this module reports.
 */
#define PIN_BOOT GPIO_NUM_0
#define PIN_PWR  GPIO_NUM_3

typedef enum {
    BTN_EVT_NONE = 0,
    BTN_BOOT_SHORT,     /* released before the long-press time */
    BTN_BOOT_LONG,      /* reported once, while still held */
    BTN_BOOT_DOUBLE,    /* two short presses within DOUBLE_PRESS_MS */
    BTN_PWR_SHORT,
    BTN_PWR_LONG,
} btn_evt_t;

void buttons_init(void);

/* Raw (undebounced) levels; used to decide the wake source after light sleep. */
bool buttons_boot_down_raw(void);
bool buttons_pwr_down_raw(void);

/* Adopt the current physical state without generating an event and swallow the press that is
 * currently held (used right after a button woke the watch). */
void buttons_sync_and_consume(void);

/* Call every ~10 ms. Returns at most one event per call. A single BOOT short press is reported
 * DOUBLE_PRESS_MS after release, once it is clear no second press follows. */
btn_evt_t buttons_poll(void);
