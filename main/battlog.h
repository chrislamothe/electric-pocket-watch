#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Battery log in its own flash partition ("battlog", 64 KB = 2048 records of 32 bytes, about
 * 21 days at one record per 15 min), so it survives the battery running flat. Each record: wall
 * clock (if synced), uptime, boot number, battery mV, PMIC gauge %, USB/charge state, and wake
 * counts since boot. The controller adds a timer wake to light sleep so samples continue while
 * the screen is off.
 *
 * Export: Settings > System > "Send log over USB" prints every record as CSV lines starting
 * with "BATTLOG," on the USB serial console. Boot records also carry the ESP32 reset reason and
 * the PMIC's power-on / power-off source registers, so the log says how the last run ended.
 */
#define BATTLOG_PERIOD_MIN     15
#define BATTLOG_LOW_PERIOD_MIN 2      /* on battery below BATTLOG_LOW_MV */
#define BATTLOG_LOW_MV         3500
#define BATTLOG_EMPTY_MV       3350   /* screen-off reading at which the watch powers itself off */

typedef enum { BATTLOG_WAKE_BUTTON, BATTLOG_WAKE_TAP, BATTLOG_WAKE_MOTION_REJECTED } battlog_wake_t;

void battlog_init(void);                          /* find the partition and the write position */
void battlog_sample(const char *why);
bool battlog_due(void);
uint64_t battlog_us_until_due(void);
void battlog_note_wake(battlog_wake_t w);
void battlog_summary(char *buf, size_t len);      /* text for Settings > System */
void battlog_dump(void);                          /* CSV over the serial console */
int battlog_recent_mv(int32_t *out, int max);     /* most recent samples' mV, oldest first */
