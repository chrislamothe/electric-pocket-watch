#pragma once

#include <stdbool.h>
#include <time.h>
#include "driver/i2c_master.h"

/*
 * PCF85063 real-time clock (I2C 0x51) on the 1.75 / 1.75-B and 2.06. It runs from the AXP2101's
 * always-on RTC supply, so it keeps time while the watch is powered off. Detected at start-up;
 * on boards without it (1.75C) every call is a no-op.
 *
 * Kept in UTC. Registers 0x04..0x0A are BCD seconds..years; seconds bit 7 (OS) is set when the
 * oscillator stopped (e.g. the battery was disconnected), meaning the time is not trustworthy.
 */
bool ext_rtc_init(i2c_master_bus_handle_t bus);
bool ext_rtc_present(void);

/* When the system clock is unset (cold boot), sets it from the RTC if the RTC holds a valid time.
 * Returns true if it did. */
bool ext_rtc_restore_system_time(void);

/* Writes the current system time, aligned to the second (after an NTP sync). Takes up to 1 s. */
void ext_rtc_save_system_time(void);

/* Steps the system clock to the RTC (the ESP32's sleep clock is an RC oscillator; the RTC is quartz).
 * Takes up to 1 s. Returns true if the clock was corrected. */
#define EXT_RTC_SYNC_PERIOD_S 3600
bool ext_rtc_sync_system_time(void);
void ext_rtc_sync_if_due(void);                 /* hourly; call from the periodic battery-log points */
