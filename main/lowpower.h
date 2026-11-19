#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Screen-off power savings and sleep accounting.
 *
 * - Display panel: deep standby while dark (DISPOFF, SLPIN, DSTB). Its memory is lost, so
 *   lowpower_screen_on() resets and re-initialises it (~0.2 s, display still off, brightness 0)
 *   and lowpower_screen_show() redraws the whole screen before DISPON.
 * - Hardware the watch never uses (touch, accelerometer, audio) is powered down once at boot by
 *   main/parked.c, not here.
 *
 * Also counts light sleep: how much of the screen-off time was really spent asleep, and how often
 * a sleep attempt was rejected. Call with the LVGL worker paused (screen off) or not yet resumed.
 */
void lowpower_screen_off(void);
void lowpower_screen_on(void);              /* panel out of deep standby, still dark */
void lowpower_screen_show(void);            /* full redraw, then DISPON; LVGL running */

void lowpower_sleep_begin(void);             /* around esp_light_sleep_start() */
void lowpower_sleep_end(bool slept);
void lowpower_off_begin(void);               /* screen-off period */
void lowpower_off_end(void);                 /* logs this period's sleep share */
void lowpower_summary(char *buf, size_t len);
