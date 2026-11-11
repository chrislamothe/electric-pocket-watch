#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Screen-off power savings for parts the watch does not need while dark.
 *
 * - Display panel: DISPOFF while dark, DISPON on wake (the frame memory is kept; no re-init). The
 *   driver's own sleep call is not used: on this board it enters deep standby, and leaving that
 *   re-runs the panel init sequence, about 1.4 s.
 * - Touch controller (CST9217): held in reset while dark (TP_RST low, latched through light sleep
 *   with gpio_hold). It restarts in its normal reporting mode when released.
 * - Speaker amplifier: its enable pin is driven low and latched, so it can never float on. The
 *   audio codecs are never initialised and stay in their power-on reset state.
 *
 * Also counts light sleep: how much of the screen-off time was really spent asleep, and how often
 * a sleep attempt was rejected. Call with the LVGL worker paused (screen off) or not yet resumed.
 */
void lowpower_init(void);
void lowpower_screen_off(void);
void lowpower_screen_on(void);

void lowpower_sleep_begin(void);             /* around esp_light_sleep_start() */
void lowpower_sleep_end(bool slept);
void lowpower_off_begin(void);               /* screen-off period */
void lowpower_off_end(void);                 /* logs this period's sleep share */
void lowpower_summary(char *buf, size_t len);
