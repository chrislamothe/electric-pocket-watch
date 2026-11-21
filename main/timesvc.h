#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/*
 * Time service: the one owner of the watch's clock.
 *
 *   - Sources, best first: network (SNTP), the PCF85063 RTC (quartz; 1.75 / 1.75-B / 2.06), and
 *     the ESP32's own clock, which runs from an internal RC oscillator while asleep (no 32 kHz
 *     crystal on these boards) and drifts by up to ~100 ppm.
 *   - Every change to the system clock and every RTC access goes through here, under one lock.
 *     net.c holds the lock across an SNTP exchange and reports the result with timesvc_net_synced().
 *   - Scheduling uses esp_timer (monotonic, kept across light sleep), never the adjustable clock.
 *   - RTC boards: the system clock is checked against the RTC on every screen wake (whole second,
 *     before the face is drawn) and corrected precisely on the RTC's seconds tick once an hour.
 *     A disagreement over 120 s is resolved, not ignored: a network time from the last 48 h wins
 *     and the RTC is rewritten; otherwise the running RTC wins.
 *   - 1.75C (no RTC): the RC drift is measured between network syncs and subtracted hourly; a
 *     saved network is used for an automatic sync once a day (once a week on RTC boards).
 *   - After a cold start with no RTC time the clock is UNSET: seeded from the firmware build time
 *     so it runs, and the faces show "Time not set" until a sync.
 */

typedef enum {
    TIME_UNSET = 0,         /* seeded from the build time: not a measurement */
    TIME_RETAINED,          /* survived an ESP32 reset; source before the reset unknown */
    TIME_RTC,               /* from the PCF85063 */
    TIME_NETWORK,           /* SNTP since the last power-on */
} time_source_t;

/* After ext_rtc_init() and settings_init(); before anything reads the time. */
void timesvc_init(void);

/* Local broken-down time and milliseconds in the second. */
void timesvc_now(struct tm *out, int *ms);
time_t timesvc_utc(void);

bool timesvc_trusted(void);
time_source_t timesvc_source(void);
uint32_t timesvc_generation(void);      /* changes whenever the clock is stepped */

/* Screen about to light: quick RTC check (RTC boards), before the face is drawn. */
void timesvc_on_wake(void);

/* Periodic work (hourly RTC correction, drift compensation, automatic sync). Non-blocking if the
 * service is busy; an RTC correction itself takes up to 1.2 s. */
void timesvc_service(void);
uint64_t timesvc_us_until_due(void);    /* for the light-sleep timer */

/* For net.c: hold across the SNTP exchange; report the result before releasing.
 * err_us = system clock minus network time just before the step (+ = clock was fast). */
bool timesvc_lock(int timeout_ms);
void timesvc_unlock(void);
int64_t timesvc_utc_us(void);
void timesvc_net_synced(int64_t err_us);

void timesvc_summary(char *buf, size_t len);
