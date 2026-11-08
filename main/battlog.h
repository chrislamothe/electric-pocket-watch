#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Battery history for working out real drain: every BATTLOG_PERIOD_MIN (also while the screen is
 * off - the controller adds a timer wake to light sleep) it records battery voltage, the PMIC
 * gauge %, charge state and wake counts. Kept in RAM (survives light sleep, lost on power-off).
 */
#define BATTLOG_PERIOD_MIN 15

typedef enum { BATTLOG_WAKE_BUTTON, BATTLOG_WAKE_TAP, BATTLOG_WAKE_MOTION_REJECTED } battlog_wake_t;

void battlog_sample(const char *why);
bool battlog_due(void);
uint64_t battlog_us_until_due(void);
void battlog_note_wake(battlog_wake_t w);
void battlog_summary(char *buf, size_t len);   /* multi-line text for Settings > System */
