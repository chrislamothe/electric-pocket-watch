#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

/*
 * PCF85063 real-time clock (I2C 0x51) on the 1.75 / 1.75-B and 2.06: quartz crystal, powered from
 * the AXP2101's always-on supply, so it keeps time while the watch is off. Absent on the 1.75C.
 *
 * Hardware access only. Which clock is trusted, when to correct and from what source is decided
 * by main/timesvc.c, which serialises every call here. Kept in UTC, 24-hour mode.
 */
bool ext_rtc_init(i2c_master_bus_handle_t bus);
bool ext_rtc_present(void);

/* Whole-second read. False if absent, unreadable, oscillator-stop flag set, stopped at boot and
 * not rewritten since, or an impossible date. */
bool ext_rtc_read(long long *utc_s);

/* Waits for the seconds register to change (up to 1.2 s) and returns the new second together with
 * the esp_timer time of that change (+-1 ms). */
bool ext_rtc_read_edge(long long *utc_s, int64_t *mono_us);

/* Loads the given UTC time, aligned to the second: the clock is stopped, loaded with second S and
 * released at S + 0.492 s (first increment 0.5078 s after release, PCF85063A datasheet 8.2.1.4),
 * then checked to have ticked to S+1. utc_now_us() supplies the reference. Up to ~1.7 s. */
bool ext_rtc_write_aligned(int64_t (*utc_now_us)(void));
