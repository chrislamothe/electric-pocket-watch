#pragma once

#include <time.h>

/* Default zone: US Pacific with automatic DST (changeable later through settings). */
#define WATCH_DEFAULT_TZ "PST8PDT,M3.2.0,M11.1.0"

/* Sets the time zone and, if the system clock is still unset (cold boot on a board without an RTC
 * chip, or an RTC that lost power), seeds it from the firmware build time (UTC) until an NTP sync
 * replaces it. Call after ext_rtc_restore_system_time(). */
void watch_time_init(void);

/* Local broken-down time and milliseconds within the current second. */
void watch_time_now(struct tm *out, int *ms);
