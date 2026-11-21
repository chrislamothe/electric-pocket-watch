#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

/*
 * PCF85063 driver (I2C 0x51; 1.75 / 1.75-B / 2.06). Hardware access only: no policy, never touches
 * the system clock. Only main/clock.c calls it, under its hardware lock.
 *
 * Every calendar access is one burst of seconds..years (registers 04h..0Ah): the chip freezes its
 * counters for the duration of an access and keeps only one pending increment, so an access must
 * be one transaction and finish well within a second (NXP PCF85063A, "Setting and reading the time").
 * Kept in UTC, 24-hour mode.
 */
typedef enum {
    RTC_OK = 0,
    RTC_ABSENT,
    RTC_BUS,            /* I2C error / timeout */
    RTC_STOPPED,        /* STOP bit set (frozen) */
    RTC_OSC_LOST,       /* OS flag: oscillator stopped since the last write; time invalid */
    RTC_BAD_DATA,       /* not BCD, impossible date, or outside 2024..2099 */
    RTC_NO_TICK,        /* seconds did not advance within the deadline */
    RTC_VERIFY,         /* write not confirmed by read-back */
} rtc_status_t;

const char *rtc_status_text(rtc_status_t s);

bool rtc_init(i2c_master_bus_handle_t bus);
bool rtc_present(void);

/* One coherent calendar read: whole UTC second (sub-second phase unknown). */
rtc_status_t rtc_read(int64_t *utc_s);

/* Polls for the seconds rollover (deadline 1.3 s): returns the new second and the esp_timer time of
 * the rollover, bracketed by the two reads around it (*window_us = bracket width). */
rtc_status_t rtc_read_edge(int64_t *utc_s, int64_t *mono_us, int64_t *window_us);

/* Writes the time given by now_us() aligned to the second (STOP, load second S, release at
 * S + 0.492 s: first increment 0.5078 s after release, PCF85063A 8.2.1.4), restarts the clock on
 * every path, then verifies: running, OS clear, and reading S+1 after the next rollover. */
rtc_status_t rtc_write(int64_t (*now_us)(void));
