#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

/*
 * Touch input, powered only while the Settings menu is open.
 *
 * The controller is asleep the rest of the time (its own deep-sleep command; a reset pulse wakes
 * it). The faces have no touch input. A small task owns the chip: it wakes it, polls it at 50 Hz
 * while active and puts it back to sleep. LVGL reads the latest point through a pointer input
 * device. If the chip does not answer, the menu still works with the BOOT button.
 *
 * The chips are read directly over I2C (CST9217 on the 1.75 boards, FT3168 on the 2.06), with the
 * register layout of Waveshare's / Espressif's drivers, so nothing here can abort the firmware.
 */

/* Call once after the display has started and before light sleep is first used: puts the chip to
 * sleep and creates the LVGL input device (takes the LVGL lock). */
void touch_init(i2c_master_bus_handle_t bus);

/* Request touch on or off. Returns at once; safe from any task and from LVGL callbacks. */
void touch_set_active(bool on);

/* Wait until the chip has reached the requested state (used before light sleep). */
void touch_wait_settled(int max_ms);

/* Times the sleep command failed after 3 attempts (shown on Settings > System). */
uint32_t touch_sleep_failures(void);

/* Puts the controller to sleep without LVGL or the touch task (clock lab firmware). */
void touch_sleep_only(i2c_master_bus_handle_t bus);
