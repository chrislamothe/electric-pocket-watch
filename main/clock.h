#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "driver/i2c_master.h"

/*
 * Clock service: the watch's one authority for the time (ground-up replacement of the v0.13/0.14
 * time code).
 *
 * MODEL. The time is an anchor - a UTC instant measured at an esp_timer instant - plus the
 * esp_timer time elapsed since. While awake that is crystal-timed (40 MHz XTAL). Across light
 * sleep esp_timer is bridged by ESP-IDF from the RTC slow clock, which on these boards is an
 * internal RC oscillator and NOT trusted. Every anchor comes from a measured reference: the
 * PCF85063 (quartz) or the network. The libc clock (gettimeofday) is only a mirror for logs.
 *
 * SOURCES.
 *   - PCF85063 (1.75 / 1.75-B / 2.06): read before every screen wake is drawn (whole second;
 *     the fraction comes from the sleep-corrected bridge if it agrees, else mid-second), then
 *     refined on the RTC's seconds rollover (+-1 ms) right after. Also at least hourly.
 *   - Network: SNTP's time hook (sntp_sync_time, weak in ESP-IDF) is overridden so SNTP hands over
 *     a measurement instead of setting the clock; the service applies it and writes the RTC,
 *     verified. RTC time is only ever written from a network reference.
 *   - 1.75C (no RTC): only the bridge, corrected for the measured sleep-clock error, and network
 *     syncs (automatic daily with a saved network).
 *
 * SLEEP ERROR. Each reference is compared with the uncorrected bridge since the previous reference;
 * the difference divided by the time spent asleep is the sleep clock's error (ppm), averaged and
 * stored, and subtracted after every sleep.
 *
 * STATES. UNSET (no measurement since power-on: the faces say so), ESTIMATED (bridged across sleep
 * with no fresh reference: 1.75C, or an RTC that failed), READY (anchored to a reference this wake).
 */

typedef enum { CLOCK_UNSET = 0, CLOCK_ESTIMATED, CLOCK_READY } clock_quality_t;
typedef enum { CLOCK_SRC_NONE = 0, CLOCK_SRC_RETAINED, CLOCK_SRC_RTC, CLOCK_SRC_NETWORK } clock_source_t;

/* After settings_init() and the I2C bus; reads the RTC (up to 1.3 s) and starts the service task. */
void clock_init(i2c_master_bus_handle_t bus);

/* Current UTC in microseconds; false when UNSET (value then meaningless). Lock-free. */
bool clock_utc_us(int64_t *out);
/* Local broken-down time; when UNSET fills 10:10:00 and returns false. */
bool clock_local(struct tm *out);
clock_quality_t clock_quality(void);
clock_source_t clock_source(void);
uint32_t clock_generation(void);        /* changes on every re-anchor */

/* Screen about to light: anchor from the RTC (RTC boards; ~1 ms) and queue the precise refinement. */
void clock_display_wake(void);

/* Around esp_light_sleep_start(). prepare() fails (do not sleep) while an RTC or network
 * transaction is running; resume() must follow every successful prepare(). */
bool clock_sleep_prepare(void);
void clock_sleep_resume(bool slept);
uint64_t clock_us_until_due(void);      /* next clock work: bound for the sleep timer */

/* SNTP, used by net.c: arm before starting SNTP, then wait for the measurement, which is applied
 * (and saved to the RTC) before returning. */
void clock_sntp_arm(void);
bool clock_sntp_wait_apply(int timeout_ms);

void clock_summary(char *buf, size_t len);
