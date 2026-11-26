#pragma once

#include <stdint.h>

/*
 * Event log for diagnosing behaviour away from a computer: the last 96 events (screen on/off with
 * reason, button presses with duration, wake sources, Wi-Fi jobs, clock re-anchors, read errors).
 *
 * Kept in RTC memory: survives resets and crashes (not power-off), costs no flash writes. Printed as
 * EVLOG lines after the battery log (Settings > System > Send log over USB) and at boot.
 */
void evlog_add(const char *what, const char *tag, int32_t a, int32_t b);
void evlog_dump(void);
